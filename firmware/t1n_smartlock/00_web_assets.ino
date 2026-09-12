// Keep embedded WebUI JavaScript in headers so Arduino's automatic
// function-prototype generator never parses JavaScript syntax.
#define MAIN_HTML LEGACY_MAIN_HTML
#define MANIFEST_JSON LEGACY_MANIFEST_JSON
#include "web_assets.h"
#undef MAIN_HTML
#undef MANIFEST_JSON
#include "pwa_assets.h"
