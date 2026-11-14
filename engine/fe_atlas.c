/* Fouille — built-in atlas (step 2). lieux.json replaces this at step 3.
 * Coordinates are neighbourhood centres, approximate (±300 m); radii cover
 * the heart of each place. Montreal first, then the autogeography. */
#include "fe_internal.h"

const fe_place FE_PLACES[] = {
    { 45.5225f, -73.5800f, 1.4f, "Plateau",       "plateau+mont-royal+montreal" },
    { 45.5245f, -73.5990f, 0.9f, "Mile End",      "mile+end+montreal" },
    { 45.4780f, -73.5850f, 1.2f, "St-Henri",      "saint-henri+montreal" },
    { 45.5446f, -73.5436f, 1.5f, "Hochelaga",     "hochelaga+montreal" },
    { 45.5075f, -73.5544f, 0.9f, "Vieux-Mtl",     "vieux-montreal+old+montreal" },
    { 45.5017f, -73.5673f, 1.2f, "Centre-ville",  "downtown+montreal" },
    { 45.4590f, -73.5700f, 1.8f, "Verdun",        "verdun+montreal" },
    { 45.5330f, -73.5980f, 1.4f, "Pte-Patrie",    "petite-patrie+montreal" },
    { 45.4720f, -73.6150f, 1.8f, "NDG",           "notre-dame-de-grace+montreal" },
    { 45.5480f, -73.6220f, 1.4f, "Villeray",      "villeray+montreal" },
    { 45.4920f, -73.5600f, 0.9f, "Griffintown",   "griffintown+montreal" },
    { 45.5160f, -73.6080f, 1.2f, "Outremont",     "outremont+montreal" },
    { 45.5300f, -73.6300f, 1.0f, "Parc-Ex",       "parc-extension+montreal" },
    { 45.5050f, -73.5880f, 1.2f, "Mont-Royal",    "mont-royal+montreal" },
    { 45.4560f, -73.6100f, 2.5f, "Canal Lachine", "lachine+canal+montreal" },
    { 45.5560f, -73.6600f, 2.0f, "Ahuntsic",      "ahuntsic+montreal" },
    { 45.5089f, -73.5617f, 8.0f, "Montreal",      "montreal" },
    { 46.8139f, -71.2080f, 5.0f, "Quebec",        "quebec+city" },
    { 48.4280f, -71.0680f, 15.0f,"Saguenay",      "saguenay" },
    { 47.5640f, -69.8670f, 20.0f,"Kamouraska",    "kamouraska" },
};
const int FE_NPLACES = (int)(sizeof(FE_PLACES) / sizeof(FE_PLACES[0]));

int fe_place_count(void) { return FE_NPLACES; }
const char *fe_place_name(int i) { return (i >= 0 && i < FE_NPLACES) ? FE_PLACES[i].name : "?"; }
int fe_place_info(int i, float *lat, float *lon, float *km) {
    if (i < 0 || i >= FE_NPLACES) return 0;
    *lat = FE_PLACES[i].lat; *lon = FE_PLACES[i].lon; *km = FE_PLACES[i].radius_km;
    return 1;
}
