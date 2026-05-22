try {
  var env = require("./env");
} catch (e) {
  console.log("Could not load env variables " + JSON.stringify(e));
  if (!env) {
    env = {};
  }
}

var TILE_URLS = {
  osm: "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
  osm_topo: 
    "https://tile.opentopomap.org/{z}/{x}/{y}.png",
  osm_cyclosm:
    "https://c.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png",
  stamen_watercolor:
    "https://tiles.stadiamaps.com/tiles/stamen_watercolor/{z}/{x}/{y}.png?api_key={STADIAMAPS_API_KEY}",
  stamen_toner:
    "https://tiles.stadiamaps.com/tiles/stamen_toner/{z}/{x}/{y}.png?api_key={STADIAMAPS_API_KEY}",
  stamen_terrain:
    "https://tiles.stadiamaps.com/tiles/stamen_terrain/{z}/{x}/{y}.png?api_key={STADIAMAPS_API_KEY}",
  positron:
    "https://a.basemaps.cartocdn.com/light_all/{z}/{x}/{y}.png",
};

function getTileUrl(provider, zoom, x, y) {
  var template = TILE_URLS[provider] || TILE_URLS.osm;
  return template
    .replace("{z}", zoom)
    .replace("{x}", x)
    .replace("{y}", y)
    .replace("{STADIAMAPS_API_KEY}", env.STADIAMAPS_API_KEY || "576c9b86-0dbc-48bc-ba62-b84600003eff");
  
}

module.exports = {
  getTileUrl: getTileUrl,
  TILE_URLS: TILE_URLS,
};
