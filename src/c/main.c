#include <pebble.h>

// 4x4 Bayer Dither Matrix for 16-level grayscale mapping
static const uint8_t __attribute__((unused)) BAYER_4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

static Window *s_window;
static Layer *s_canvas_layer;
static TextLayer *s_status_layer;

static bool s_js_ready;
static bool s_show_time_overlay;
static bool s_phone_connected;

static uint8_t *s_image_buffer;
static size_t s_image_buffer_size;
static size_t s_received_bytes;
static uint16_t s_image_width;
static uint16_t s_image_height;
static uint16_t s_image_row_bytes;
static bool s_device_is_color;
static bool s_image_is_color;
static bool s_image_ready;
static uint8_t s_compression_format;
static AppTimer *s_retry_timer;
static AppTimer *s_time_overlay_timer;

enum
{
  CMD_INIT = 1,
  CMD_IMAGE_CHUNK = 2,
  CMD_BUTTON_CLICK = 3,
  CMD_SAVE_SETTINGS = 4,
  CMD_UPDATE_TIME_OVERLAY = 5,
};

bool comm_is_js_ready()
{
  return s_js_ready;
}

static void prv_schedule_time_overlay_tick(void);

static void prv_set_status_text(const char *text)
{
  if (!s_status_layer)
  {
    return;
  }
  text_layer_set_text(s_status_layer, text);
  layer_set_hidden(text_layer_get_layer(s_status_layer), false);
}

static void prv_draw_time_overlay(Layer *layer, GContext *ctx)
{
  if (!s_show_time_overlay)
  {
    return;
  }

  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  if (!tick_time)
  {
    return;
  }

  char time_text[6];
  strftime(time_text, sizeof(time_text), "%H:%M", tick_time);

  GRect bounds = layer_get_bounds(layer);
  const int16_t overlay_h = 22;
  GRect overlay_bounds = GRect(bounds.size.w / 2 - 20, 0, 40, overlay_h);
  GRect text_bounds = GRect(0, -1, bounds.size.w, overlay_h + 1);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, overlay_bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(
      ctx,
      time_text,
      fonts_get_system_font(FONT_KEY_GOTHIC_18),
      text_bounds,
      GTextOverflowModeTrailingEllipsis,
      GTextAlignmentCenter,
      NULL);
}

static void prv_time_overlay_timer_handler(void *context)
{
  s_time_overlay_timer = NULL;
  if (s_canvas_layer && s_show_time_overlay)
  {
    layer_mark_dirty(s_canvas_layer);
  }
  prv_schedule_time_overlay_tick();
}

static void prv_schedule_time_overlay_tick(void)
{
  if (s_time_overlay_timer)
  {
    app_timer_cancel(s_time_overlay_timer);
    s_time_overlay_timer = NULL;
  }

  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  uint32_t delay_ms = 60000;
  if (tick_time)
  {
    delay_ms = (uint32_t)(60 - tick_time->tm_sec) * 1000;
    if (delay_ms == 0)
    {
      delay_ms = 60000;
    }
  }

  s_time_overlay_timer = app_timer_register(delay_ms, prv_time_overlay_timer_handler, NULL);
}

static void prv_reset_image_state(void)
{
  s_received_bytes = 0;
  s_image_ready = false;
  s_compression_format = 0;
  if (s_image_buffer)
  {
    free(s_image_buffer);
    s_image_buffer = NULL;
    s_image_buffer_size = 0;
  }
}

static bool prv_decode_color_rle2_to_raw(const uint8_t *src,
                                         size_t src_len,
                                         uint8_t *dst,
                                         size_t dst_len)
{
  size_t si = 0;
  size_t di = 0;

  while (si < src_len && di < dst_len)
  {
    uint8_t packed = src[si++];
    uint8_t run = (uint8_t)((packed >> 6) & 0x03) + 1;
    uint8_t color = (uint8_t)(0xC0 | (packed & 0x3F));

    for (uint8_t i = 0; i < run && di < dst_len; i++)
    {
      dst[di++] = color;
    }
  }

  return di == dst_len;
}

static bool prv_decode_mono_bitrle2_to_packed(const uint8_t *src,
                                              size_t src_len,
                                              uint8_t *dst,
                                              uint16_t width,
                                              uint16_t height,
                                              uint16_t row_bytes)
{
  if (src_len < 1) return false;

  memset(dst, 0, (size_t)row_bytes * height);

  const uint8_t start_color = src[0] & 0x01;
  uint8_t color = start_color;
  uint32_t total_pixels = (uint32_t)width * (uint32_t)height;
  uint32_t pixel_index = 0;
  uint32_t bit_pos = 8;
  uint32_t total_bits = (uint32_t)src_len * 8;

  while (pixel_index < total_pixels)
  {
    if (bit_pos + 2 > total_bits) return false;

    uint8_t token = 0;
    for (uint8_t i = 0; i < 2; i++)
    {
      uint8_t byte = src[bit_pos >> 3];
      uint8_t bit = (byte >> (7 - (bit_pos & 7))) & 0x01;
      token = (uint8_t)((token << 1) | bit);
      bit_pos++;
    }

    uint32_t run = token + 1;
    bool toggle_after_run = true;
    if (token == 3)
    {
      if (bit_pos + 8 > total_bits) return false;

      uint8_t ext = 0;
      for (uint8_t i = 0; i < 8; i++)
      {
        uint8_t byte = src[bit_pos >> 3];
        uint8_t bit = (byte >> (7 - (bit_pos & 7))) & 0x01;
        ext = (uint8_t)((ext << 1) | bit);
        bit_pos++;
      }
      if (ext == 0)
      {
        run = 258;
        toggle_after_run = false;
      }
      else
      {
        run = (uint32_t)ext + 3;
      }
    }

    for (uint32_t i = 0; i < run && pixel_index < total_pixels; i++)
    {
      if (color)
      {
        uint16_t x = (uint16_t)(pixel_index % width);
        uint16_t y = (uint16_t)(pixel_index / width);
        size_t byte_index = (size_t)y * row_bytes + (x >> 3);
        uint8_t bit_index = (uint8_t)(7 - (x & 7));
        dst[byte_index] |= (uint8_t)(1u << bit_index);
      }
      pixel_index++;
    }

    if (toggle_after_run)
    {
      color = (uint8_t)(color ? 0 : 1);
    }
  }

  return true;
}

static void prv_request_render(void)
{
  if (!s_phone_connected) return;

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK)
  {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Failed to begin AppMessage outbox.");
    return;
  }

  dict_write_uint8(iter, MESSAGE_KEY_cmd, CMD_INIT);
  dict_write_uint16(iter, MESSAGE_KEY_width, s_image_width);
  dict_write_uint16(iter, MESSAGE_KEY_height, s_image_height);
  dict_write_uint16(iter, MESSAGE_KEY_bytes_per_row, s_image_row_bytes);
  dict_write_uint8(iter, MESSAGE_KEY_is_color, s_device_is_color ? 1 : 0);

  AppMessageResult result = app_message_outbox_send();
  if (result != APP_MSG_OK)
  {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Error sending the outbox: %d", (int)result);
  }
}

static void prv_retry_timer_handler(void *context)
{
  s_retry_timer = NULL;
  prv_request_render();
}

static void prv_phone_connection_handler(bool connected)
{
  s_phone_connected = connected;

  if (!connected)
  {
    if (s_retry_timer)
    {
      app_timer_cancel(s_retry_timer);
      s_retry_timer = NULL;
    }
    prv_reset_image_state();
    prv_set_status_text("No phone connection");
    if (s_canvas_layer)
    {
      layer_mark_dirty(s_canvas_layer);
    }
    return;
  }
  else
  {
    if (!s_image_ready)
    {
      prv_set_status_text("Loading map...");
    }
    prv_request_render();
  }
}

static void prv_inbox_received_handler(DictionaryIterator *iter, void *context)
{
  Tuple *cmd_t = dict_find(iter, MESSAGE_KEY_cmd);
  if (!cmd_t) return;

  APP_LOG(APP_LOG_LEVEL_INFO, "Inbox message received, cmd: %d", cmd_t->value->uint8);
  if (cmd_t->value->uint8 == CMD_INIT)
  {
    Tuple *ready_t = dict_find(iter, MESSAGE_KEY_JSReady);
    if (ready_t)
    {
      s_js_ready = ready_t->value->uint8 != 0;
      Tuple *show_time_t = dict_find(iter, MESSAGE_KEY_showTimeOverlay);
      if (show_time_t)
      {
        s_show_time_overlay = show_time_t->value->uint8 != 0;
        layer_mark_dirty(s_canvas_layer);
      }
      Tuple *canvas_supported_t = dict_find(iter, MESSAGE_KEY_isCanvasSupported);
      if (canvas_supported_t && canvas_supported_t->value->uint8)
      {
        prv_request_render();
      }
      else
      {
        text_layer_set_text(s_status_layer, "Canvas not supported :(");
      }
    }
    return;
  }
  else if (cmd_t->value->uint8 == CMD_IMAGE_CHUNK)
  {
    Tuple *width_t = dict_find(iter, MESSAGE_KEY_width);
    Tuple *height_t = dict_find(iter, MESSAGE_KEY_height);
    Tuple *row_bytes_t = dict_find(iter, MESSAGE_KEY_bytes_per_row);
    Tuple *is_color_t = dict_find(iter, MESSAGE_KEY_is_color);
    Tuple *compression_t = dict_find(iter, MESSAGE_KEY_compression_format);
    Tuple *total_t = dict_find(iter, MESSAGE_KEY_total_bytes);
    Tuple *offset_t = dict_find(iter, MESSAGE_KEY_chunk_offset);
    Tuple *data_t = dict_find(iter, MESSAGE_KEY_chunk_data);

    if (!offset_t || !data_t) return;

    if (width_t) s_image_width = width_t->value->uint16;
    if (height_t) s_image_height = height_t->value->uint16;
    if (row_bytes_t) s_image_row_bytes = row_bytes_t->value->uint16;
    if (is_color_t) s_image_is_color = is_color_t->value->uint8 != 0;
    if (compression_t) s_compression_format = compression_t->value->uint8;

    if (total_t && (!s_image_buffer || s_image_buffer_size != total_t->value->uint32))
    {
      prv_reset_image_state();
      if (compression_t) s_compression_format = compression_t->value->uint8;
      
      s_image_buffer_size = total_t->value->uint32;
      s_image_buffer = malloc(s_image_buffer_size);
      
      // OUT OF MEMORY CHECK #1
      if (!s_image_buffer)
      {
        s_image_buffer_size = 0;
        APP_LOG(APP_LOG_LEVEL_ERROR, "OOM: map buffer size %d", (int)total_t->value->uint32);
        prv_set_status_text("Error: Out of memory!");
        return;
      }
      memset(s_image_buffer, 0, s_image_buffer_size);
    }

    if (!s_image_buffer) return;

    const uint32_t offset = offset_t->value->uint32;
    const uint16_t length = data_t->length;
    if (offset + length > s_image_buffer_size) return;

    memcpy(s_image_buffer + offset, data_t->value->data, length);
    s_received_bytes += length;

    if (s_received_bytes >= s_image_buffer_size)
    {
      // No secondary malloc here. We keep s_image_buffer as is.
      s_image_ready = true;
      layer_set_hidden(text_layer_get_layer(s_status_layer), true);
      layer_mark_dirty(s_canvas_layer);
    }
  } else if (cmd_t->value->uint8 == CMD_UPDATE_TIME_OVERLAY)
  {
    Tuple *show_time_t = dict_find(iter, MESSAGE_KEY_showTimeOverlay);
    if (show_time_t)
    {
      s_show_time_overlay = show_time_t->value->uint8 != 0;
      layer_mark_dirty(s_canvas_layer);
    }
  }
}

static void prv_outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context)
{
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage outbox failed to send: %d", (int)reason);
  if (s_phone_connected && !s_retry_timer)
  {
    s_retry_timer = app_timer_register(1000, prv_retry_timer_handler, NULL);
  }
}

static void prv_canvas_update_proc(Layer *layer, GContext *ctx)
{
  GRect bounds = layer_get_bounds(layer);
  if (!s_image_ready || !s_image_buffer) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    return;
  }

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;

  uint8_t *fb_data = gbitmap_get_data(fb);
  const uint16_t fb_row_bytes = gbitmap_get_bytes_per_row(fb);

for (uint16_t y = 0; y < s_image_height && y < bounds.size.h; y++)
  {
    uint8_t *dst_row = fb_data + (y * fb_row_bytes);

#if defined(PBL_BW)
    // On B&W watches, start the row out as Black (0)
    memset(dst_row, 0, fb_row_bytes);
#endif

    for (uint16_t x = 0; x < s_image_width && x < bounds.size.w; x++) {
        uint32_t pixel_idx = (uint32_t)(y * s_image_width) + x;

#if defined(PBL_COLOR)
        // --- COLOR WATCHES ---
        if (s_image_is_color && s_compression_format == 0) {
            // Write 8-bit Pebble Color directly to the frame buffer
            dst_row[x] = s_image_buffer[pixel_idx];
        } else {
            // Fallback for 4-bit grayscale on color watches
            uint8_t byte = s_image_buffer[pixel_idx / 2];
            uint8_t val = (pixel_idx % 2 == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
            
            if (val > 11) dst_row[x] = GColorWhiteARGB8;
            else if (val > 7) dst_row[x] = GColorLightGrayARGB8;
            else if (val > 3) dst_row[x] = GColorDarkGrayARGB8;
            else dst_row[x] = GColorBlackARGB8;
        }
#else
        // --- B&W WATCHES (Aplite / Pebble Steel) ---
        uint8_t byte = s_image_buffer[pixel_idx / 2];
        uint8_t val = (pixel_idx % 2 == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
      


        // HYBRID EDGE-PRESERVING DITHER
        if (val <= 3) {
            // 1. HARD BLACK: Dark pixels (roads, labels, borders) stay solid BLACK.
            // (We do nothing here because memset already set the row to 0/Black).
        //} else if (val >= 13) {
        //    // 2. HARD WHITE: Light pixels (map background) stay crisp WHITE.
        //    // This prevents "dirty" dots in empty spaces.
        //    dst_row[x >> 3] |= (1 << (x & 7));
        } else {
            // 3. DITHERED GREY: Mid-tones (parks, water, shading) are dithered.
            // The 4x4 matrix distributes the dots smoothly.
            uint8_t threshold = BAYER_4x4[(y % 4) * 4 + (x % 4)];
            if (val > threshold) {
                dst_row[x >> 3] |= (1 << (x & 7)); // Set White
            }
        }
#endif
    }
  }
  graphics_release_frame_buffer(ctx, fb);
  prv_draw_time_overlay(layer, ctx);
}

static void prv_window_load(Window *window)
{
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_canvas_layer = layer_create(bounds);
  layer_set_update_proc(s_canvas_layer, prv_canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  s_status_layer = text_layer_create(GRect(0, (bounds.size.h / 2) - 10, bounds.size.w, 20));
  text_layer_set_text(s_status_layer, s_phone_connected ? "Loading map..." : "No phone connection");
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_layer, GColorClear);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  // --- REPLACE FROM HERE ---
  s_image_width = (uint16_t)bounds.size.w;
  s_image_height = (uint16_t)bounds.size.h;
  
  // Force row bytes to be equal to width so the buffer is a simple 2D array
  // This eliminates the 'gaps' or 'staggered pixels'
  s_image_row_bytes = s_image_width; 

  s_device_is_color = false;
#if defined(PBL_COLOR)
  s_device_is_color = true;
#endif
  s_image_is_color = s_device_is_color;
  // --- TO HERE ---

  prv_reset_image_state();
  prv_schedule_time_overlay_tick();
  if (s_retry_timer)
  {
    app_timer_cancel(s_retry_timer);
    s_retry_timer = NULL;
  }
}

static void prv_window_unload(Window *window)
{
  if (s_js_ready)
  {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK)
    {
      dict_write_uint8(iter, MESSAGE_KEY_cmd, CMD_SAVE_SETTINGS);
      app_message_outbox_send();
    }
  }

  text_layer_destroy(s_status_layer);
  layer_destroy(s_canvas_layer);
  if (s_retry_timer)
  {
    app_timer_cancel(s_retry_timer);
    s_retry_timer = NULL;
  }
  if (s_time_overlay_timer)
  {
    app_timer_cancel(s_time_overlay_timer);
    s_time_overlay_timer = NULL;
  }
  prv_reset_image_state();
}

static void prv_focus_handler(bool in_focus)
{
  if (!in_focus) return;
  
  s_phone_connected = connection_service_peek_pebble_app_connection();
  prv_phone_connection_handler(s_phone_connected);
}

static void send_click(int which)
{
  if (!s_phone_connected)
  {
    prv_set_status_text("No phone connection");
    return;
  }

  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  dict_write_uint8(iter, MESSAGE_KEY_cmd, CMD_BUTTON_CLICK);
  dict_write_int(iter, MESSAGE_KEY_button_id, &which, sizeof(int), true);
  dict_write_end(iter);
  app_message_outbox_send();
}

static void click_up_handler(ClickRecognizerRef recognizer, void *context) { send_click(-1); }
static void click_sel_handler(ClickRecognizerRef recognizer, void *context) { send_click(0); }
static void click_down_handler(ClickRecognizerRef recognizer, void *context) { send_click(1); }

static void click_config_provider(void *context)
{
  window_single_click_subscribe(BUTTON_ID_UP, click_up_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, click_sel_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, click_down_handler);
}

static void prv_init(void)
{
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
                                             .load = prv_window_load,
                                             .unload = prv_window_unload,
                                         });

  app_message_register_inbox_received(prv_inbox_received_handler);
  app_message_register_outbox_failed(prv_outbox_failed_handler);
  
  uint32_t inbox = app_message_inbox_size_maximum();
  uint32_t outbox = app_message_outbox_size_maximum();
  
  // Cap AppMessage buffers on memory-constrained Aplite platforms
#if defined(PBL_PLATFORM_APLITE)
  if (inbox > 2048)  inbox = 2048;
  if (outbox > 512) outbox = 512;
#endif

  app_message_open(inbox, outbox);

  connection_service_subscribe((ConnectionHandlers){
      .pebble_app_connection_handler = prv_phone_connection_handler,
  });
  s_phone_connected = connection_service_peek_pebble_app_connection();
  app_focus_service_subscribe(prv_focus_handler);

  const bool animated = true;
  window_stack_push(s_window, animated);

  prv_phone_connection_handler(s_phone_connected);
  window_set_click_config_provider(s_window, click_config_provider);
}

static void prv_deinit(void)
{
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void)
{
  prv_init();
  app_event_loop();
  prv_deinit();
}