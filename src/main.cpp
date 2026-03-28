#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <HTTPUpdate.h>
#include <vector>
#include <map>
#include <time.h>

// ── Firmware version ────────────────────────────────────────────────
#define FW_VERSION "1.0.0"
#define OTA_VERSION_URL "https://raw.githubusercontent.com/blumleo2004/linetracker/main/version.json"
static const unsigned long OTA_CHECK_INTERVAL_MS = 6UL * 60 * 60 * 1000; // 6h

// ── Display ──────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);

// Fahrgastinformationssystem: amber LEDs on dark matte display
static uint16_t BG_COLOR;    // dark warm matte grey (display cover)
static uint16_t AMBER;       // bright amber LED color (#FFBF00)
static uint16_t AMBER_DIM;   // dim amber for separators/glow

// ── Config ───────────────────────────────────────────────────────────
static const char* CONFIG_PATH = "/config.json";
static const char* WIFI_RESET_FLAG = "/wifi_reset";

// CSV cache on SPIFFS
static const char* CACHE_HALT_PATH   = "/halt.csv";
static const char* CACHE_STEIGE_PATH = "/steige.csv";
static const char* CACHE_LINIEN_PATH = "/linien.csv";
static const char* CACHE_TS_PATH     = "/cache_ts";
static const unsigned long CACHE_MAX_AGE_MS = 24UL * 60 * 60 * 1000; // 24h

static int  cfgRotateSec      = 5;     // page switch interval in seconds (default 5)
static int  cfgBrightness     = 255;   // backlight 0-255 (default max)
static int  cfgNightFrom      = -1;    // night mode start hour (0-23), -1 = disabled
static int  cfgNightTo        = -1;    // night mode end hour (0-23)
static int  cfgNightBright    = 20;    // backlight during night mode
static bool cfgShowNext       = false; // show next departure below main countdown
static bool cfgShowDisruptions= false; // show WL disruption ticker at bottom

struct ConfigLine {
    String rbl;
    String name;
    String towards;
    String type;
};
static std::vector<ConfigLine> cfgLines;

struct OebbStation {
    String stationName;  // canonical ÖBB station name (e.g. "Wien Rennweg")
    String line;         // e.g. "S 3", "REX 7"
    String towards;      // destination direction
};
static std::vector<OebbStation> cfgOebb;

bool loadConfig() {
    if (!SPIFFS.exists(CONFIG_PATH)) return false;
    File f = SPIFFS.open(CONFIG_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();

    cfgLines.clear();
    cfgOebb.clear();

    if (doc["lines"].is<JsonArray>()) {
        for (JsonObject line : doc["lines"].as<JsonArray>()) {
            ConfigLine cl;
            cl.rbl     = line["rbl"].as<String>();
            cl.name    = line["name"] | "";
            cl.towards = line["towards"] | "";
            cl.type    = line["type"] | "";
            if (cl.rbl.length() > 0) cfgLines.push_back(cl);
        }
    } else if (doc["rbls"].is<String>()) {
        String rbls = doc["rbls"].as<String>();
        while (rbls.length() > 0) {
            int comma = rbls.indexOf(',');
            String rbl;
            if (comma == -1) { rbl = rbls; rbls = ""; }
            else { rbl = rbls.substring(0, comma); rbls = rbls.substring(comma + 1); }
            rbl.trim();
            if (rbl.length() > 0) { ConfigLine cl; cl.rbl = rbl; cfgLines.push_back(cl); }
        }
    }
    if (doc["oebb"].is<JsonArray>()) {
        for (JsonObject stn : doc["oebb"].as<JsonArray>()) {
            OebbStation os;
            os.stationName = stn["station"].as<String>();
            os.line        = stn["line"] | "";
            os.towards     = stn["towards"] | "";
            if (os.stationName.length() > 0) cfgOebb.push_back(os);
        }
    }
    cfgRotateSec       = doc["rotate_sec"]        | 5;
    cfgBrightness      = doc["brightness"]        | 255;
    cfgNightFrom       = doc["night_from"]        | -1;
    cfgNightTo         = doc["night_to"]          | -1;
    cfgNightBright     = doc["night_bright"]      | 20;
    cfgShowNext        = doc["show_next"]         | false;
    cfgShowDisruptions = doc["show_disruptions"]  | false;
    if (cfgRotateSec   < 2)   cfgRotateSec   = 2;
    if (cfgRotateSec   > 60)  cfgRotateSec   = 60;
    if (cfgBrightness  < 10)  cfgBrightness  = 10;
    if (cfgBrightness  > 255) cfgBrightness  = 255;
    if (cfgNightBright < 0)   cfgNightBright = 0;
    if (cfgNightBright > 255) cfgNightBright = 255;

    return cfgLines.size() > 0 || cfgOebb.size() > 0;
}

void saveConfig() {
    File f = SPIFFS.open(CONFIG_PATH, "w");
    if (!f) return;
    JsonDocument doc;
    JsonArray arr = doc["lines"].to<JsonArray>();
    for (auto& cl : cfgLines) {
        JsonObject obj = arr.add<JsonObject>();
        obj["rbl"]     = cl.rbl;
        obj["name"]    = cl.name;
        obj["towards"] = cl.towards;
        obj["type"]    = cl.type;
    }
    JsonArray oArr = doc["oebb"].to<JsonArray>();
    for (auto& os : cfgOebb) {
        JsonObject obj = oArr.add<JsonObject>();
        obj["station"] = os.stationName;
        obj["line"]    = os.line;
        obj["towards"] = os.towards;
    }
    doc["rotate_sec"]       = cfgRotateSec;
    doc["brightness"]       = cfgBrightness;
    doc["night_from"]       = cfgNightFrom;
    doc["night_to"]         = cfgNightTo;
    doc["night_bright"]     = cfgNightBright;
    doc["show_next"]        = cfgShowNext;
    doc["show_disruptions"] = cfgShowDisruptions;
    serializeJson(doc, f);
    f.close();
}

// ── Data model ───────────────────────────────────────────────────────
struct Departure {
    String lineName;
    String towards;
    String type;
    int    countdown;
    bool   realtime;
};

static std::vector<Departure> departures;     // all departures (raw)
static std::vector<Departure> displaySlots;   // one per line+direction (smart grouped)
static std::vector<String>    disruptions;    // active WL disruption titles
static SemaphoreHandle_t dataMutex;
static const unsigned long FETCH_INTERVAL_MS = 20000;
static bool fetchError = false;
static volatile bool configChanged = false;   // triggers immediate refetch

// ── Config Web Server ────────────────────────────────────────────────
WebServer server(80);
static bool configMode = false;

// ── CSV Cache ────────────────────────────────────────────────────────
bool downloadCsvToCache(const char* url, const char* path) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(20000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    File f = SPIFFS.open(path, "w");
    if (!f) { http.end(); return false; }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[512];
    while (stream->available()) {
        int len = stream->readBytes(buf, sizeof(buf));
        f.write(buf, len);
    }
    f.close();
    http.end();
    return true;
}

bool isCacheValid() {
    if (!SPIFFS.exists(CACHE_TS_PATH)) return false;
    File f = SPIFFS.open(CACHE_TS_PATH, "r");
    if (!f) return false;
    String ts = f.readString();
    f.close();
    unsigned long cached = strtoul(ts.c_str(), NULL, 10);
    return (millis() - cached) < CACHE_MAX_AGE_MS;
}

void saveCacheTimestamp() {
    File f = SPIFFS.open(CACHE_TS_PATH, "w");
    if (f) { f.print(millis()); f.close(); }
}

bool refreshCsvCache(bool force = false) {
    if (!force && isCacheValid()
        && SPIFFS.exists(CACHE_HALT_PATH)
        && SPIFFS.exists(CACHE_STEIGE_PATH)
        && SPIFFS.exists(CACHE_LINIEN_PATH)) {
        Serial.println("CSV cache valid, skipping download");
        return true;
    }
    Serial.println("Downloading CSV cache...");
    bool ok = true;
    ok &= downloadCsvToCache("https://data.wien.gv.at/csv/wienerlinien-ogd-haltestellen.csv", CACHE_HALT_PATH);
    ok &= downloadCsvToCache("https://data.wien.gv.at/csv/wienerlinien-ogd-steige.csv", CACHE_STEIGE_PATH);
    ok &= downloadCsvToCache("https://data.wien.gv.at/csv/wienerlinien-ogd-linien.csv", CACHE_LINIEN_PATH);
    if (ok) {
        saveCacheTimestamp();
        Serial.println("CSV cache updated");
    } else {
        Serial.println("CSV cache download failed (partial)");
    }
    return ok;
}

// ── Struct for discovered lines at a station ─────────────────────────
struct FoundLine {
    String rbl;
    String lineName;
    String towards;
    String type;
    String stopName;
};

// Look up line name and type from linien cache by LINIEN_ID
// Linien CSV: "LINIEN_ID";"BEZEICHNUNG";"ECHTZEIT";"VERKEHRSMITTEL";"STAND"
bool lookupLineInfo(const String& linienId, String& outName, String& outType) {
    File f = SPIFFS.open(CACHE_LINIEN_PATH, "r");
    if (!f) return false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        int s1 = line.indexOf(';');
        if (s1 < 0) continue;
        String id = line.substring(0, s1);
        id.replace("\"", "");
        if (id != linienId) continue;
        // BEZEICHNUNG
        int s2 = line.indexOf(';', s1 + 1);
        if (s2 < 0) { f.close(); return false; }
        outName = line.substring(s1 + 1, s2);
        outName.replace("\"", "");
        // skip ECHTZEIT
        int s3 = line.indexOf(';', s2 + 1);
        if (s3 < 0) { f.close(); return false; }
        // VERKEHRSMITTEL
        int s4 = line.indexOf(';', s3 + 1);
        String vm = (s4 > 0) ? line.substring(s3 + 1, s4) : line.substring(s3 + 1);
        vm.replace("\"", "");
        vm.trim();
        outType = vm;
        f.close();
        return true;
    }
    f.close();
    return false;
}

// Search stations from cached haltestellen CSV
std::vector<std::pair<String,String>> searchStations(const String& query) {
    std::vector<std::pair<String,String>> results;

    File f = SPIFFS.open(CACHE_HALT_PATH, "r");
    if (!f) return results;  // cache missing

    String lowerQuery = query;
    lowerQuery.toLowerCase();

    while (f.available()) {
        String line = f.readStringUntil('\n');
        String lowerLine = line;
        lowerLine.toLowerCase();
        if (lowerLine.indexOf(lowerQuery) >= 0) {
            // CSV: "HALTESTELLEN_ID";"TYP";"DIVA";"NAME";"GEMEINDE";...
            int s1 = line.indexOf(';');
            if (s1 < 0) continue;
            String haltId = line.substring(0, s1);
            haltId.replace("\"", "");
            int s2 = line.indexOf(';', s1 + 1);
            if (s2 < 0) continue;
            int s3 = line.indexOf(';', s2 + 1);
            if (s3 < 0) continue;
            int s4 = line.indexOf(';', s3 + 1);
            if (s4 < 0) continue;
            String name = line.substring(s3 + 1, s4);
            name.replace("\"", "");
            if (name.length() > 0 && haltId.length() > 0) {
                results.push_back({haltId, name});
            }
        }
        if (results.size() >= 10) break;
    }
    f.close();
    return results;
}

// Steige CSV: "STEIG_ID";"FK_LINIEN_ID";"FK_HALTESTELLEN_ID";"RICHTUNG";"REIHENFOLGE";"RBL_NUMMER";...
struct SteigeInfo {
    String rbl;
    String linienId;
    String richtung;  // "H" or "R"
};

// Find all RBLs (with line IDs) for a given station ID from cached steige CSV
std::vector<SteigeInfo> findSteigeForStation(const String& haltId) {
    std::vector<SteigeInfo> results;

    File f = SPIFFS.open(CACHE_STEIGE_PATH, "r");
    if (!f) return results;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.indexOf(haltId) < 0) continue;
        int s1 = line.indexOf(';');
        if (s1 < 0) continue;
        int s2 = line.indexOf(';', s1 + 1);
        if (s2 < 0) continue;
        int s3 = line.indexOf(';', s2 + 1);
        if (s3 < 0) continue;
        String foundHalt = line.substring(s2 + 1, s3);
        foundHalt.replace("\"", "");
        if (foundHalt != haltId) continue;

        // RICHTUNG
        int s4 = line.indexOf(';', s3 + 1);
        if (s4 < 0) continue;
        String richtung = line.substring(s3 + 1, s4);
        richtung.replace("\"", "");

        // skip REIHENFOLGE
        int s5 = line.indexOf(';', s4 + 1);
        if (s5 < 0) continue;
        // RBL_NUMMER
        int s6 = line.indexOf(';', s5 + 1);
        if (s6 < 0) continue;
        String rbl = line.substring(s5 + 1, s6);
        rbl.replace("\"", "");
        rbl.trim();

        // FK_LINIEN_ID
        String linienId = line.substring(s1 + 1, s2);
        linienId.replace("\"", "");

        if (rbl.length() > 0) {
            bool dup = false;
            for (auto& r : results) { if (r.rbl == rbl) { dup = true; break; } }
            if (!dup) results.push_back({rbl, linienId, richtung});
        }
    }
    f.close();
    return results;
}

// Legacy wrapper: return just RBL strings
std::vector<String> findRblsForStation(const String& haltId) {
    auto steige = findSteigeForStation(haltId);
    std::vector<String> rbls;
    for (auto& s : steige) rbls.push_back(s.rbl);
    return rbls;
}

// Query the monitor API to see what lines/directions are at given RBLs
std::vector<FoundLine> probeRbls(const std::vector<String>& rbls) {
    std::vector<FoundLine> results;
    if (rbls.empty()) return results;

    for (size_t start = 0; start < rbls.size(); start += 10) {
        String url = "https://www.wienerlinien.at/ogd_realtime/monitor?activateTrafficInfo=stoerunglang";
        size_t end = min(start + 10, rbls.size());
        for (size_t i = start; i < end; i++) {
            url += "&rbl=" + rbls[i];
        }

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, url);
        http.setTimeout(15000);
        int code = http.GET();
        if (code != 200) { http.end(); continue; }

        String payload = http.getString();
        http.end();

        JsonDocument filter;
        filter["data"]["monitors"][0]["locationStop"]["properties"]["title"] = true;
        filter["data"]["monitors"][0]["locationStop"]["properties"]["attributes"]["rbl"] = true;
        filter["data"]["monitors"][0]["lines"][0]["name"] = true;
        filter["data"]["monitors"][0]["lines"][0]["towards"] = true;
        filter["data"]["monitors"][0]["lines"][0]["type"] = true;

        JsonDocument doc;
        if (deserializeJson(doc, payload, DeserializationOption::Filter(filter),
                            DeserializationOption::NestingLimit(20))) continue;

        JsonArray monitors = doc["data"]["monitors"].as<JsonArray>();
        for (JsonObject mon : monitors) {
            String stopName = mon["locationStop"]["properties"]["title"].as<String>();
            String rbl = String((int)mon["locationStop"]["properties"]["attributes"]["rbl"]);
            JsonArray lines = mon["lines"].as<JsonArray>();
            for (JsonObject line : lines) {
                FoundLine fl;
                fl.rbl = rbl;
                fl.lineName = line["name"].as<String>();
                fl.towards = line["towards"].as<String>();
                fl.type = line["type"].as<String>();
                fl.stopName = stopName;
                bool dup = false;
                for (auto& r : results) {
                    if (r.rbl == fl.rbl && r.lineName == fl.lineName && r.towards == fl.towards) {
                        dup = true; break;
                    }
                }
                if (!dup) results.push_back(fl);
            }
        }
    }
    return results;
}

// ── HTML Templates ───────────────────────────────────────────────────
const char HTML_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>LineTracker</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d0d1a;color:#eee;padding:0 16px 16px;max-width:500px;margin:0 auto;-webkit-font-smoothing:antialiased}
header{text-align:center;padding:20px 0 12px}
header h1{color:#ffbf00;font-size:1.6em;letter-spacing:1px;margin:0}
header p{color:#666;font-size:11px;margin-top:2px}
h2{color:#ffbf00;font-size:1em;margin:14px 0 10px;letter-spacing:.5px;text-transform:uppercase;font-weight:600}
.card{background:linear-gradient(145deg,#141428,#181830);border:1px solid #222;border-radius:14px;padding:16px;margin-bottom:14px;box-shadow:0 2px 12px rgba(0,0,0,.3)}
input[type=text],input[type=number]{width:100%;padding:12px;border-radius:10px;border:1px solid #2a2a4a;background:#0a0a1e;color:#fff;font-size:16px;transition:border .2s}
input[type=text]:focus,input[type=number]:focus{outline:none;border-color:#ffbf00}
input[type=number]{width:60px;padding:8px;text-align:center}
input[type=range]{width:100%;margin:8px 0;accent-color:#ffbf00;height:6px}
button{background:linear-gradient(135deg,#e94560,#d63352);color:#fff;border:none;padding:12px 24px;border-radius:10px;font-size:15px;font-weight:600;cursor:pointer;width:100%;margin-top:8px;transition:transform .1s,box-shadow .2s;box-shadow:0 2px 8px rgba(233,69,96,.25)}
button:hover{transform:translateY(-1px);box-shadow:0 4px 12px rgba(233,69,96,.35)}
button:active{transform:translateY(0)}
button.add{background:linear-gradient(135deg,#1a8f3c,#158a35);box-shadow:0 2px 8px rgba(26,143,60,.25)}
button.add:hover{box-shadow:0 4px 12px rgba(26,143,60,.35)}
.line-item{display:flex;align-items:center;gap:10px;padding:12px 8px;border-bottom:1px solid rgba(255,255,255,.05);transition:background .15s}
.line-item:last-child{border-bottom:none}
.line-item:hover{background:rgba(255,191,0,.03);border-radius:8px}
.badge{display:inline-block;padding:5px 12px;border-radius:8px;color:#fff;font-weight:700;min-width:44px;text-align:center;font-size:13px;letter-spacing:.5px;box-shadow:0 1px 4px rgba(0,0,0,.3)}
.badge.metro{background:linear-gradient(135deg,#c9935a,#a87040)}
.badge.tram{background:linear-gradient(135deg,#e94560,#c73652)}
.badge.bus{background:linear-gradient(135deg,#1a6fc4,#1456a0)}
.badge.train{background:linear-gradient(135deg,#388e3c,#2e7032)}
.badge.unknown{background:#555}
.dir{flex:1;font-size:14px;line-height:1.3}
.stop{font-size:11px;color:#666;margin-top:2px}
input[type=checkbox]{width:20px;height:20px;accent-color:#ffbf00;cursor:pointer}
.status{text-align:center;color:#666;padding:20px;font-size:14px}
.rm{background:linear-gradient(135deg,#c0392b,#a93226);width:36px;min-width:36px;padding:8px 0;font-size:16px;margin:0;border-radius:50%;box-shadow:0 1px 4px rgba(0,0,0,.3)}
.rm:hover{background:linear-gradient(135deg,#e74c3c,#c0392b)}
.msg{background:linear-gradient(135deg,#1a8f3c,#158a35);border-radius:10px;padding:12px;margin:8px 0;text-align:center;font-weight:600}
.letters{display:flex;flex-wrap:wrap;gap:5px;justify-content:center;margin:10px 0}
.letters a{display:inline-block;width:33px;height:33px;line-height:33px;text-align:center;background:#141428;color:#ffbf00;border-radius:8px;text-decoration:none;font-weight:700;font-size:13px;border:1px solid #222;transition:all .15s}
.letters a:hover,.letters a.act{background:#ffbf00;color:#0d0d1a;border-color:#ffbf00}
.stn{display:block;padding:11px 8px;border-bottom:1px solid rgba(255,255,255,.05);color:#eee;text-decoration:none;font-size:15px;transition:all .15s;border-radius:6px}
.stn:hover{background:rgba(255,191,0,.06);padding-left:14px}
label{display:block}
</style></head><body>
<header><h1>LineTracker</h1><p>by Leo Blum</p></header>
)rawliteral";

String badgeClassForType(const String& type) {
    if (type == "ptMetro") return "badge metro";
    if (type == "ptTram")  return "badge tram";
    if (type.startsWith("ptBus")) return "badge bus";
    if (type == "ptTrainS") return "badge train";
    return "badge unknown";
}

void handleRoot() {
    String html = FPSTR(HTML_HEAD);


    // Show success message if redirected after save
    if (server.hasArg("saved")) {
        html += "<div class='msg'>Linien gespeichert!</div>";
    }

    // Show currently configured lines with badges and individual remove
    html += "<div class='card'><h2>Aktive Linien</h2>";
    if (cfgLines.empty()) {
        html += "<p class='status'>Noch keine Linien konfiguriert.<br>Suche unten eine Station.</p>";
    } else {
        for (size_t i = 0; i < cfgLines.size(); i++) {
            auto& cl = cfgLines[i];
            html += "<div class='line-item'>";
            html += "<span class='" + badgeClassForType(cl.type) + "'>" + cl.name + "</span>";
            html += "<div class='dir'>" + cl.towards + "</div>";
            html += "<form action='/remove' method='POST' style='margin:0'>";
            html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
            html += "<button class='rm'>&#x2715;</button></form>";
            html += "</div>";
        }
    }
    html += "</div>";

    // ÖBB stations section
    html += "<div class='card'><h2>S-Bahn / Zuege (OeBB)</h2>";
    if (cfgOebb.empty()) {
        html += "<p class='status'>Noch keine OeBB-Linien konfiguriert.</p>";
    } else {
        for (size_t i = 0; i < cfgOebb.size(); i++) {
            auto& os = cfgOebb[i];
            html += "<div class='line-item'>";
            html += "<span class='badge train'>" + os.line + "</span>";
            html += "<div><div class='dir'>" + os.towards + "</div>";
            html += "<div class='stop'>" + os.stationName + "</div></div>";
            html += "<form action='/oebb-remove' method='POST' style='margin:0'>";
            html += "<input type='hidden' name='idx' value='" + String(i) + "'>";
            html += "<button class='rm'>&#x2715;</button></form>";
            html += "</div>";
        }
    }
    html += "<form action='/oebb-search' method='GET'>";
    html += "<input type='text' name='q' placeholder='z.B. Wien Rennweg, Meidling...'>";
    html += "<button class='add' type='submit'>OeBB-Station suchen</button>";
    html += "</form></div>";

    // Search form
    html += "<div class='card'><h2>Station suchen</h2>";
    html += "<form action='/search' method='GET'>";
    html += "<input type='text' name='q' placeholder='z.B. Kutschkergasse, Volksoper...' autofocus>";
    html += "<button type='submit'>Suchen</button>";
    html += "</form>";
    html += "<a href='/browse'><button style='background:#0f3460;margin-top:6px'>Alle Stationen durchblaettern</button></a>";
    html += "</div>";

    // Display settings
    html += "<div class='card'><h2>Einstellungen</h2>";
    html += "<form action='/settings' method='POST'>";

    html += "<label style='font-size:13px;color:#aaa'>Seitenwechsel: <b id='v_rot' style='color:#eee'>" + String(cfgRotateSec) + " Sek</b></label>";
    html += "<input type='range' name='rotate_sec' min='2' max='30' value='" + String(cfgRotateSec) + "' oninput=\"document.getElementById('v_rot').textContent=this.value+' Sek'\" style='width:100%;margin:8px 0'>";

    html += "<label style='font-size:13px;color:#aaa'>Helligkeit: <b id='v_bri' style='color:#eee'>" + String(cfgBrightness * 100 / 255) + "%</b></label>";
    html += "<input type='range' name='brightness' min='10' max='255' value='" + String(cfgBrightness) + "' oninput=\"document.getElementById('v_bri').textContent=Math.round(this.value*100/255)+'%'\" style='width:100%;margin:8px 0'>";

    // Night mode
    String nightFromVal = (cfgNightFrom >= 0) ? String(cfgNightFrom) : "22";
    String nightToVal   = (cfgNightTo   >= 0) ? String(cfgNightTo)   : "7";
    String nightBrVal   = String(cfgNightBright * 100 / 255);
    bool nightOn = (cfgNightFrom >= 0);
    html += "<div style='margin-top:12px'>";
    html += "<label style='font-size:13px;color:#aaa'><input type='checkbox' name='night_on' value='1' id='cb_night'";
    if (nightOn) html += " checked";
    html += " style='width:auto;margin-right:6px' onchange=\"document.getElementById('night_opts').style.display=this.checked?'block':'none'\">Nachtmodus</label></div>";

    html += "<div id='night_opts' style='display:";
    html += nightOn ? "block" : "none";
    html += "'>";
    html += "<div style='display:flex;gap:8px;margin:8px 0;font-size:13px;color:#aaa;align-items:center'>";
    html += "<span>Von</span><input type='number' name='night_from' min='0' max='23' value='" + nightFromVal + "' style='width:60px;padding:6px'>";
    html += "<span>bis</span><input type='number' name='night_to' min='0' max='23' value='" + nightToVal + "' style='width:60px;padding:6px'><span>Uhr</span></div>";
    html += "<label style='font-size:13px;color:#aaa'>Nacht-Helligkeit: <b id='v_nbri' style='color:#eee'>" + nightBrVal + "%</b></label>";
    html += "<input type='range' name='night_bright' min='0' max='100' value='" + nightBrVal + "' oninput=\"document.getElementById('v_nbri').textContent=this.value+'%'\" style='width:100%;margin:8px 0'>";
    html += "</div>";

    // Extra features
    html += "<div style='margin-top:12px;display:flex;flex-direction:column;gap:8px'>";
    html += "<label style='font-size:13px;color:#aaa'><input type='checkbox' name='show_next' value='1'";
    if (cfgShowNext) html += " checked";
    html += " style='width:auto;margin-right:6px'>Naechste Abfahrt anzeigen <span style='color:#666'>(z.B. &gt;12 min)</span></label>";
    html += "<label style='font-size:13px;color:#aaa'><input type='checkbox' name='show_disruptions' value='1'";
    if (cfgShowDisruptions) html += " checked";
    html += " style='width:auto;margin-right:6px'>Stoerungsticker anzeigen</label>";
    html += "</div>";

    html += "<button type='submit' style='background:#555;margin-top:12px'>Speichern</button>";
    html += "</form></div>";

    // WiFi settings
    html += "<div class='card'><h2>WiFi</h2>";
    html += "<p style='font-size:13px;color:#aaa;margin-bottom:8px'>Verbunden mit: <b style='color:#eee'>" + WiFi.SSID() + "</b></p>";
    html += "<form action='/wifi-reset' method='POST'>";
    html += "<button style='background:#e67e22'>WLAN aendern</button>";
    html += "</form></div>";

    // Firmware info + update
    html += "<div class='card'><h2>Firmware</h2>";
    html += "<p style='font-size:13px;color:#aaa;margin-bottom:8px'>Version: <b style='color:#eee'>v" + String(FW_VERSION) + "</b></p>";
    html += "<a href='/update'><button style='background:#0f3460'>Nach Update suchen</button></a>";
    html += "</div>";

    // Credits footer
    html += "<div style='text-align:center;margin:20px 0 8px;font-size:11px;color:#555'>";
    html += "<b style='color:#ffbf00'>LineTracker</b> by Leo Blum<br>";
    html += "Daten: <a href='https://data.wien.gv.at' style='color:#666'>Stadt Wien &ndash; data.wien.gv.at</a> (CC BY 4.0)";
    html += " &middot; <a href='https://fahrplan.oebb.at' style='color:#666'>OeBB/SCOTTY</a>";
    html += "</div>";

    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleSearch() {
    String query = server.arg("q");
    if (query.length() < 2) {
        server.sendHeader("Location", "/");
        server.send(302);
        return;
    }

    tft.fillScreen(BG_COLOR);
    tft.setTextColor(AMBER);
    tft.setTextFont(1);
    tft.setTextSize(2);
    tft.setCursor(10, 70);
    tft.print("Suche: " + query + "...");

    auto stations = searchStations(query);

    if (stations.empty()) {
        String html = FPSTR(HTML_HEAD);
    
        html += "<div class='card'><p class='status'>Keine Station gefunden fuer: " + query + "</p>";
        html += "<a href='/'><button>Zurueck</button></a></div></body></html>";
        server.send(200, "text/html", html);
        return;
    }

    // Collect all steige info (RBL + line ID + direction) from cached CSVs
    std::vector<SteigeInfo> allSteige;
    String firstStationName = stations[0].second;
    for (auto& st : stations) {
        auto steige = findSteigeForStation(st.first);
        for (auto& s : steige) {
            bool dup = false;
            for (auto& a : allSteige) { if (a.rbl == s.rbl) { dup = true; break; } }
            if (!dup) allSteige.push_back(s);
        }
    }

    // Collect RBL list for realtime probe
    std::vector<String> allRbls;
    for (auto& s : allSteige) allRbls.push_back(s.rbl);

    tft.fillScreen(BG_COLOR);
    tft.setCursor(10, 70);
    tft.print("Lade Linien...");

    // Try realtime API for currently running lines
    auto lines = probeRbls(allRbls);

    // Find RBLs that the realtime probe missed (lines not currently running)
    // and add them from CSV data so they're always visible
    for (auto& si : allSteige) {
        bool foundInProbe = false;
        for (auto& fl : lines) {
            if (fl.rbl == si.rbl) { foundInProbe = true; break; }
        }
        if (foundInProbe) continue;

        // Look up line name and type from linien CSV
        String lineName, lineType;
        if (lookupLineInfo(si.linienId, lineName, lineType)) {
            FoundLine fl;
            fl.rbl = si.rbl;
            fl.lineName = lineName;
            fl.towards = (si.richtung == "H") ? "Richtung A" : "Richtung B";
            fl.type = lineType;
            fl.stopName = firstStationName + " (faehrt gerade nicht)";
            lines.push_back(fl);
        }
    }

    String html = FPSTR(HTML_HEAD);

    html += "<div class='card'><h2>Ergebnisse: " + query + "</h2>";

    if (lines.empty()) {
        html += "<p class='status'>Keine Linien gefunden</p>";
    } else {
        html += "<form action='/save' method='POST'>";
        for (size_t i = 0; i < lines.size(); i++) {
            auto& fl = lines[i];
            bool alreadyActive = false;
            for (auto& cl : cfgLines) {
                if (cl.rbl == fl.rbl) { alreadyActive = true; break; }
            }

            String val = fl.rbl + "|" + fl.lineName + "|" + fl.towards + "|" + fl.type;
            html += "<div class='line-item'>";
            html += "<input type='checkbox' name='line' value='" + val + "'";
            if (alreadyActive) html += " checked disabled";
            html += ">";
            html += "<span class='" + badgeClassForType(fl.type) + "'>" + fl.lineName + "</span>";
            html += "<div><div class='dir'>" + fl.towards + "</div>";
            html += "<div class='stop'>" + fl.stopName + "</div></div>";
            html += "</div>";
        }
        html += "<button class='add' type='submit'>Ausgewaehlte hinzufuegen</button>";
        html += "</form>";
    }

    html += "<br><a href='/'><button>Zurueck</button></a></div></body></html>";
    server.send(200, "text/html", html);
}

void handleSave() {
    for (int i = 0; i < server.args(); i++) {
        if (server.argName(i) == "line") {
            String val = server.arg(i);
            // Parse: rbl|name|towards|type
            int p1 = val.indexOf('|');
            int p2 = val.indexOf('|', p1 + 1);
            int p3 = val.indexOf('|', p2 + 1);
            if (p1 < 0 || p2 < 0 || p3 < 0) continue;

            ConfigLine cl;
            cl.rbl     = val.substring(0, p1);
            cl.name    = val.substring(p1 + 1, p2);
            cl.towards = val.substring(p2 + 1, p3);
            cl.type    = val.substring(p3 + 1);

            // Skip duplicates
            bool dup = false;
            for (auto& existing : cfgLines) {
                if (existing.rbl == cl.rbl) { dup = true; break; }
            }
            if (!dup && cl.rbl.length() > 0) cfgLines.push_back(cl);
        }
    }
    saveConfig();
    configChanged = true;

    server.sendHeader("Location", "/?saved=1");
    server.send(302);
}

void handleRemove() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        if (idx >= 0 && idx < (int)cfgLines.size()) {
            cfgLines.erase(cfgLines.begin() + idx);
        }
    }
    saveConfig();
    // Clear stale display data immediately
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    departures.clear();
    displaySlots.clear();
    xSemaphoreGive(dataMutex);
    configChanged = true;
    server.sendHeader("Location", "/");
    server.send(302);
}

// Forward declarations
std::vector<Departure> fetchOebbStation(const String& stationName, int nowMin);
void applyNightMode();
String sanitize(String s);
extern bool lastNightState;

// Search ÖBB for station name, returns canonical name or empty string
String searchOebbStation(const String& query) {
    String encoded = query;
    encoded.replace(" ", "+");
    String url = "https://fahrplan.oebb.at/bin/ajax-getstop.exe/dn"
                 "?start=1&tpl=stop2json&REQ0JourneyStopsS0A=1"
                 "&REQ0JourneyStopsS0G=" + encoded + "&js=true&";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) { http.end(); return ""; }
    String payload = http.getString();
    http.end();

    // Response: SLs.sls={"suggestions":[{"value":"Wien Rennweg",...}]};SLs.showSuggestion();
    int eqIdx = payload.indexOf('=');
    if (eqIdx < 0) return "";
    int scIdx = payload.indexOf(';', eqIdx);
    String jsonStr = (scIdx > 0) ? payload.substring(eqIdx + 1, scIdx) : payload.substring(eqIdx + 1);
    jsonStr.trim();

    JsonDocument doc;
    if (deserializeJson(doc, jsonStr, DeserializationOption::NestingLimit(10))) return "";

    JsonArray suggestions = doc["suggestions"].as<JsonArray>();
    if (suggestions.isNull() || suggestions.size() == 0) return "";

    String name = suggestions[0]["value"].as<String>();
    name.trim();
    return name;
}

void handleOebbSearch() {
    String query = server.arg("q");
    query.trim();
    if (query.length() < 2) {
        server.sendHeader("Location", "/");
        server.send(302);
        return;
    }

    tft.fillScreen(BG_COLOR);
    tft.setTextColor(AMBER);
    tft.setTextFont(1);
    tft.setTextSize(2);
    tft.setCursor(10, 70);
    tft.print("OeBB Suche...");

    // Find canonical station name
    String stationName = searchOebbStation(query);
    if (stationName.length() == 0) {
        String html = FPSTR(HTML_HEAD);
    
        html += "<div class='card'><p class='status'>OeBB-Station nicht gefunden: " + query + "</p>";
        html += "<a href='/'><button>Zurueck</button></a></div></body></html>";
        server.send(200, "text/html", html);
        return;
    }

    tft.setCursor(10, 95);
    tft.print("Lade Abfahrten...");

    // Fetch departures to discover lines/directions
    struct tm timeinfo;
    int nowMin = 0;
    if (getLocalTime(&timeinfo, 1000)) nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    auto allDeps = fetchOebbStation(stationName, nowMin);

    // Deduplicate line+direction combos
    std::vector<std::pair<String,String>> combos;  // line, towards
    for (auto& d : allDeps) {
        bool dup = false;
        for (auto& c : combos) {
            if (c.first == d.lineName && c.second == d.towards) { dup = true; break; }
        }
        if (!dup) combos.push_back(std::make_pair(d.lineName, d.towards));
    }

    String html = FPSTR(HTML_HEAD);

    html += "<div class='card'><h2>" + stationName + "</h2>";

    if (combos.empty()) {
        html += "<p class='status'>Keine Abfahrten gefunden</p>";
    } else {
        html += "<form action='/oebb-save' method='POST'>";
        html += "<input type='hidden' name='station' value='" + stationName + "'>";
        for (auto& c : combos) {
            bool active = false;
            for (auto& os : cfgOebb) {
                if (os.stationName == stationName && os.line == c.first && os.towards == c.second) {
                    active = true; break;
                }
            }
            String val = c.first + "|" + c.second;
            html += "<div class='line-item'>";
            html += "<input type='checkbox' name='entry' value='" + val + "'";
            if (active) html += " checked disabled";
            html += ">";
            html += "<span class='badge train'>" + c.first + "</span>";
            html += "<div class='dir'>" + c.second + "</div>";
            html += "</div>";
        }
        html += "<button class='add' type='submit'>Ausgewaehlte hinzufuegen</button>";
        html += "</form>";
    }

    html += "<br><a href='/'><button>Zurueck</button></a></div></body></html>";
    server.send(200, "text/html", html);
}

void handleOebbSave() {
    String stationName = server.arg("station");
    for (int i = 0; i < server.args(); i++) {
        if (server.argName(i) != "entry") continue;
        String val = server.arg(i);
        int sep = val.indexOf('|');
        if (sep < 0) continue;
        String line    = val.substring(0, sep);
        String towards = val.substring(sep + 1);

        bool dup = false;
        for (auto& os : cfgOebb) {
            if (os.stationName == stationName && os.line == line && os.towards == towards) {
                dup = true; break;
            }
        }
        if (!dup) {
            OebbStation os;
            os.stationName = stationName;
            os.line        = line;
            os.towards     = towards;
            cfgOebb.push_back(os);
        }
    }
    saveConfig();
    configChanged = true;
    server.sendHeader("Location", "/?saved=1");
    server.send(302);
}

void handleOebbRemove() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        if (idx >= 0 && idx < (int)cfgOebb.size()) {
            cfgOebb.erase(cfgOebb.begin() + idx);
        }
    }
    saveConfig();
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    departures.clear();
    displaySlots.clear();
    xSemaphoreGive(dataMutex);
    configChanged = true;
    server.sendHeader("Location", "/");
    server.send(302);
}

void handleBrowse() {
    String html = FPSTR(HTML_HEAD);

    html += "<div class='card'><h2>Stationen A-Z</h2>";

    // Letter navigation
    String letter = server.arg("l");
    letter.toUpperCase();
    html += "<div class='letters'>";
    for (char c = 'A'; c <= 'Z'; c++) {
        String l(c);
        String cls = (letter == l) ? " class='act'" : "";
        html += "<a href='/browse?l=" + l + "'" + cls + ">" + l + "</a>";
    }
    html += "</div>";

    if (letter.length() == 1) {
        // Read from cached haltestellen CSV
        File f = SPIFFS.open(CACHE_HALT_PATH, "r");

        if (f) {
            std::vector<String> names;
            std::map<String, bool> seen;

            while (f.available()) {
                String line = f.readStringUntil('\n');
                int s1 = line.indexOf(';');
                if (s1 < 0) continue;
                int s2 = line.indexOf(';', s1 + 1);
                if (s2 < 0) continue;
                int s3 = line.indexOf(';', s2 + 1);
                if (s3 < 0) continue;
                int s4 = line.indexOf(';', s3 + 1);
                if (s4 < 0) continue;
                String name = line.substring(s3 + 1, s4);
                name.replace("\"", "");
                if (name.length() == 0) continue;

                String firstChar = name.substring(0, 1);
                firstChar.toUpperCase();
                if (firstChar != letter) continue;

                if (seen.find(name) != seen.end()) continue;
                seen[name] = true;
                names.push_back(name);
            }
            f.close();

            std::sort(names.begin(), names.end());

            if (names.empty()) {
                html += "<p class='status'>Keine Stationen mit " + letter + "</p>";
            } else {
                html += "<p style='color:#888;font-size:12px;margin:4px 0'>" + String(names.size()) + " Stationen</p>";
                for (auto& n : names) {
                    String encoded = n;
                    encoded.replace(" ", "+");
                    html += "<a class='stn' href='/search?q=" + encoded + "'>" + n + "</a>";
                }
            }
        } else {
            html += "<p class='status'>Stationsdaten nicht verfuegbar. Bitte neu starten.</p>";
        }
    } else {
        html += "<p class='status'>Waehle einen Buchstaben</p>";
    }

    html += "<br><a href='/'><button>Zurueck</button></a></div></body></html>";
    server.send(200, "text/html", html);
}

void handleWifiReset() {
    // Set flag so setup() opens config portal on restart
    File f = SPIFFS.open(WIFI_RESET_FLAG, "w");
    if (f) { f.print("1"); f.close(); }

    String html = FPSTR(HTML_HEAD);
    html += "<h1>WiFi Reset</h1>";
    html += "<div class='card'><p class='status'>Monitor startet neu...<br><br>";
    html += "Verbinde dich mit:<br><b style='color:#ffbf00;font-size:1.3em'>LineTracker</b><br><br>";
    html += "Waehle dein WLAN, dann oeffne:<br><b style='color:#ffbf00'>linetracker.local</b></p></div></body></html>";
    server.send(200, "text/html", html);
    delay(1500);
    ESP.restart();
}

void handleSettings() {
    if (server.hasArg("rotate_sec")) {
        cfgRotateSec = server.arg("rotate_sec").toInt();
        if (cfgRotateSec < 2)  cfgRotateSec = 2;
        if (cfgRotateSec > 60) cfgRotateSec = 60;
    }
    if (server.hasArg("brightness")) {
        cfgBrightness = server.arg("brightness").toInt();
        if (cfgBrightness < 10)  cfgBrightness = 10;
        if (cfgBrightness > 255) cfgBrightness = 255;
    }
    // Night mode
    bool nightOn = server.hasArg("night_on");
    if (nightOn) {
        cfgNightFrom = server.arg("night_from").toInt();
        cfgNightTo   = server.arg("night_to").toInt();
        // night_bright slider is 0-100 percent
        int pct      = server.arg("night_bright").toInt();
        cfgNightBright = pct * 255 / 100;
    } else {
        cfgNightFrom = -1;
        cfgNightTo   = -1;
    }
    cfgShowNext        = server.hasArg("show_next");
    cfgShowDisruptions = server.hasArg("show_disruptions");

    // Apply brightness immediately
    if (cfgNightFrom >= 0 && cfgNightTo >= 0) {
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            int h = ti.tm_hour;
            bool isNight;
            if (cfgNightFrom <= cfgNightTo) isNight = (h >= cfgNightFrom && h < cfgNightTo);
            else isNight = (h >= cfgNightFrom || h < cfgNightTo);
            ledcWrite(0, isNight ? cfgNightBright : cfgBrightness);
            lastNightState = isNight;
        }
    } else {
        ledcWrite(0, cfgBrightness);
        lastNightState = false;
    }

    saveConfig();
    server.sendHeader("Location", "/?saved=1");
    server.send(302);
}

// ── OTA Update ──────────────────────────────────────────────────────
// Compare semantic versions: returns true if remote > local
bool isNewerVersion(const String& remote, const String& local) {
    int rMaj = 0, rMin = 0, rPat = 0;
    int lMaj = 0, lMin = 0, lPat = 0;
    sscanf(remote.c_str(), "%d.%d.%d", &rMaj, &rMin, &rPat);
    sscanf(local.c_str(),  "%d.%d.%d", &lMaj, &lMin, &lPat);
    if (rMaj != lMaj) return rMaj > lMaj;
    if (rMin != lMin) return rMin > lMin;
    return rPat > lPat;
}

// Check for OTA update, returns true if update was started
bool checkOtaUpdate() {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, OTA_VERSION_URL);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, payload)) return false;

    String remoteVer = doc["version"] | "";
    String binUrl    = doc["url"] | "";
    if (remoteVer.length() == 0 || binUrl.length() == 0) return false;

    if (!isNewerVersion(remoteVer, FW_VERSION)) {
        Serial.println("OTA: up to date (v" + String(FW_VERSION) + ")");
        return false;
    }

    Serial.println("OTA: new version " + remoteVer + " available, updating...");

    // Show update progress on display
    tft.fillScreen(BG_COLOR);
    tft.setTextColor(AMBER, BG_COLOR);
    tft.setTextFont(1);
    tft.setTextSize(2);
    tft.setCursor(10, 40);
    tft.print("Firmware Update...");
    tft.setTextSize(1);
    tft.setCursor(10, 80);
    tft.print("v" + String(FW_VERSION) + " -> v" + remoteVer);
    tft.setCursor(10, 100);
    tft.print("Nicht ausschalten!");

    WiFiClientSecure updClient;
    updClient.setInsecure();

    // Follow redirects (GitHub releases redirect to S3)
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    t_httpUpdate_return ret = httpUpdate.update(updClient, binUrl);

    switch (ret) {
        case HTTP_UPDATE_OK:
            Serial.println("OTA: success, rebooting");
            ESP.restart();
            break;
        case HTTP_UPDATE_FAILED:
            Serial.printf("OTA: failed (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("OTA: no update needed");
            break;
    }
    return false;
}

void handleOtaCheck() {
    String html = FPSTR(HTML_HEAD);
    html += "<h1>Firmware Update</h1>";
    html += "<div class='card'><p class='status'>Suche nach Updates...<br>Aktuelle Version: v" + String(FW_VERSION) + "</p></div></body></html>";
    server.send(200, "text/html", html);
    // Run update check after sending response
    delay(100);
    if (!checkOtaUpdate()) {
        // No update found or failed — redirect back
        // (if update succeeds, ESP restarts before reaching here)
    }
}

void startConfigServer() {
    server.on("/", handleRoot);
    server.on("/search", handleSearch);
    server.on("/browse", handleBrowse);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/remove", HTTP_POST, handleRemove);
    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/oebb-search", handleOebbSearch);
    server.on("/oebb-save", HTTP_POST, handleOebbSave);
    server.on("/oebb-remove", HTTP_POST, handleOebbRemove);
    server.on("/wifi-reset", HTTP_POST, handleWifiReset);
    server.on("/update", handleOtaCheck);
    server.begin();
    configMode = true;
}

// ── WiFiManager ──────────────────────────────────────────────────────
void showWifiSetupScreen(bool isReset) {
    tft.fillScreen(BG_COLOR);
    tft.setTextColor(AMBER, BG_COLOR);
    tft.setTextFont(1);
    tft.setTextSize(3);
    tft.setCursor(10, 10);
    tft.print(isReset ? "WLAN aendern" : "WiFi Setup");
    tft.setTextSize(2);
    tft.setCursor(10, 50);
    tft.print("1) Connect to WiFi:");
    tft.setTextColor(TFT_YELLOW, BG_COLOR);
    tft.setCursor(10, 75);
    tft.print("  \"LineTracker\"");
    tft.setTextColor(AMBER, BG_COLOR);
    tft.setCursor(10, 105);
    tft.print("2) Choose your network");
    if (isReset) {
        tft.setTextSize(1);
        tft.setCursor(10, 140);
        tft.print("(Timeout 3min -> altes WLAN)");
    }
}

void setupWiFi() {
    WiFiManager wm;
    bool wifiResetRequested = SPIFFS.exists(WIFI_RESET_FLAG);
    if (wifiResetRequested) {
        SPIFFS.remove(WIFI_RESET_FLAG);
    }

    if (wifiResetRequested) {
        // User requested WiFi change — open portal directly
        // Old credentials stay in NVS as fallback
        showWifiSetupScreen(true);
        wm.setConfigPortalTimeout(180);  // 3 min timeout
        if (!wm.startConfigPortal("LineTracker")) {
            // Timeout — no new WiFi selected, try old credentials
            tft.fillScreen(BG_COLOR);
            tft.setTextColor(AMBER, BG_COLOR);
            tft.setTextFont(1);
            tft.setTextSize(2);
            tft.setCursor(10, 70);
            tft.print("Verbinde mit altem WLAN...");
            WiFi.begin();  // Uses stored NVS credentials
            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
                delay(500);
            }
            if (WiFi.status() != WL_CONNECTED) {
                // Old WiFi also gone — restart and try again
                ESP.restart();
            }
        }
    } else {
        // Normal startup — auto-connect to saved WiFi, portal if needed
        showWifiSetupScreen(false);
        wm.setConfigPortalTimeout(180);
        if (!wm.autoConnect("LineTracker")) {
            delay(3000);
            ESP.restart();
        }
    }
}

// ── API fetch ────────────────────────────────────────────────────────
String buildUrl() {
    String url = "https://www.wienerlinien.at/ogd_realtime/monitor?activateTrafficInfo=stoerunglang";
    for (auto& cl : cfgLines) {
        url += "&rbl=" + cl.rbl;
    }
    return url;
}

void fetchDepartures() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (cfgLines.empty()) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, buildUrl());
    http.setTimeout(15000);
    int code = http.GET();

    if (code != 200) { http.end(); fetchError = true; return; }

    String payload = http.getString();
    http.end();

    JsonDocument filter;
    filter["data"]["monitors"][0]["lines"][0]["name"] = true;
    filter["data"]["monitors"][0]["lines"][0]["towards"] = true;
    filter["data"]["monitors"][0]["lines"][0]["type"] = true;
    filter["data"]["monitors"][0]["lines"][0]["realtimeSupported"] = true;
    filter["data"]["monitors"][0]["lines"][0]["departures"]["departure"][0]["departureTime"]["countdown"] = true;
    filter["data"]["trafficInfos"][0]["title"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, payload, DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(20))) {
        fetchError = true;
        return;
    }

    std::vector<Departure> newDeps;
    JsonArray monitors = doc["data"]["monitors"].as<JsonArray>();
    for (JsonObject monitor : monitors) {
        JsonArray lines = monitor["lines"].as<JsonArray>();
        for (JsonObject line : lines) {
            String name    = line["name"].as<String>();
            String towards = line["towards"].as<String>();
            String type    = line["type"].as<String>();
            bool   rt      = line["realtimeSupported"] | false;
            JsonArray deps = line["departures"]["departure"].as<JsonArray>();
            for (JsonObject dep : deps) {
                Departure d;
                d.lineName  = name;
                d.towards   = towards;
                d.type      = type;
                d.countdown = dep["departureTime"]["countdown"] | -1;
                d.realtime  = rt;
                if (d.countdown >= 0) newDeps.push_back(d);
            }
        }
    }

    // Sort all by countdown
    std::sort(newDeps.begin(), newDeps.end(),
        [](const Departure& a, const Departure& b) { return a.countdown < b.countdown; });

    // ── Smart grouping: one slot per line+direction (soonest departure) ──
    std::vector<Departure> newSlots;
    std::map<String, bool> seen;
    for (auto& d : newDeps) {
        String key = d.lineName + "|" + d.towards;
        if (seen.find(key) == seen.end()) {
            newSlots.push_back(d);
            seen[key] = true;
        }
    }

    // Parse disruptions
    std::vector<String> newDisruptions;
    if (doc["data"]["trafficInfos"].is<JsonArray>()) {
        for (JsonObject info : doc["data"]["trafficInfos"].as<JsonArray>()) {
            String title = info["title"].as<String>();
            title.trim();
            if (title.length() > 0) newDisruptions.push_back(sanitize(title));
        }
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    departures   = newDeps;
    displaySlots = newSlots;
    disruptions  = newDisruptions;
    fetchError   = false;
    xSemaphoreGive(dataMutex);
}

// ── ÖBB S-Bahn / train fetch ─────────────────────────────────────────
String decodeHtmlEntities(String s) {
    s.replace("&#228;", "ä"); s.replace("&#246;", "ö");
    s.replace("&#252;", "ü"); s.replace("&#196;", "Ä");
    s.replace("&#214;", "Ö"); s.replace("&#220;", "Ü");
    s.replace("&#223;", "ß"); s.replace("&#38;", "&");
    s.replace("&amp;", "&"); s.replace("&lt;", "<");
    s.replace("&gt;", ">"); s.replace("&quot;", "\"");
    return s;
}

// Fetch ÖBB departures for a single station, return raw list
std::vector<Departure> fetchOebbStation(const String& stationName, int nowMin) {
    std::vector<Departure> result;
    String encoded = stationName;
    encoded.replace(" ", "+");

    String url = "https://fahrplan.oebb.at/bin/stboard.exe/dn"
                 "?L=vs_scotty.vs_liveticker"
                 "&input=" + encoded +
                 "&boardType=dep"
                 "&productsFilter=0000110000"
                 "&maxJourneys=20"
                 "&outputMode=tickerDataOnly"
                 "&start=yes&time=now&selectDate=today";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) { http.end(); return result; }

    String payload = http.getString();
    http.end();

    int eqIdx = payload.indexOf('=');
    if (eqIdx < 0) return result;
    String jsonStr = payload.substring(eqIdx + 1);
    jsonStr.trim();
    if (jsonStr.endsWith(";")) jsonStr = jsonStr.substring(0, jsonStr.length() - 1);

    JsonDocument doc;
    if (deserializeJson(doc, jsonStr, DeserializationOption::NestingLimit(15))) return result;

    JsonArray journeys = doc["journey"].as<JsonArray>();
    if (journeys.isNull()) return result;

    for (JsonObject j : journeys) {
        String line = j["pr"].as<String>();
        String dest = j["st"].as<String>();
        String ti   = j["ti"].as<String>();
        dest = decodeHtmlEntities(dest);
        line = decodeHtmlEntities(line);
        line.trim(); dest.trim();

        String depTime = ti;
        if (j["rt"].is<JsonObject>()) {
            String rtTime = j["rt"]["dlt"].as<String>();
            if (rtTime.length() >= 5) depTime = rtTime;
        }

        int col = depTime.indexOf(':');
        if (col < 0) continue;
        int depMin = depTime.substring(0, col).toInt() * 60 + depTime.substring(col + 1).toInt();
        int diff = depMin - nowMin;
        if (diff < -60) diff += 1440;
        if (diff < 0) continue;

        Departure d;
        d.lineName  = line;
        d.towards   = dest;
        d.type      = "ptTrainS";
        d.countdown = diff;
        d.realtime  = j["rt"].is<JsonObject>();
        result.push_back(d);
    }
    return result;
}

void fetchOebbDepartures() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (cfgOebb.empty()) return;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5000)) {
        Serial.println("OeBB: NTP time not available");
        return;
    }
    int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    // Collect unique station names to avoid duplicate API calls
    std::map<String, std::vector<Departure>> stationCache;
    for (auto& os : cfgOebb) {
        if (stationCache.find(os.stationName) == stationCache.end()) {
            stationCache[os.stationName] = fetchOebbStation(os.stationName, nowMin);
        }
    }

    // Filter cached results by configured line+direction
    std::vector<Departure> oebbDeps;
    for (auto& os : cfgOebb) {
        auto& allDeps = stationCache[os.stationName];
        for (auto& d : allDeps) {
            if (d.lineName == os.line && d.towards == os.towards) {
                oebbDeps.push_back(d);
            }
        }
    }

    if (oebbDeps.empty()) return;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    for (auto& d : oebbDeps) departures.push_back(d);
    std::sort(departures.begin(), departures.end(),
        [](const Departure& a, const Departure& b) { return a.countdown < b.countdown; });
    displaySlots.clear();
    std::map<String, bool> seen;
    for (auto& d : departures) {
        String key = d.lineName + "|" + d.towards;
        if (seen.find(key) == seen.end()) {
            displaySlots.push_back(d);
            seen[key] = true;
        }
    }
    xSemaphoreGive(dataMutex);
}

// ── German chars to ASCII ────────────────────────────────────────────
String sanitize(String s) {
    s.replace("ä", "ae"); s.replace("Ä", "Ae");
    s.replace("ö", "oe"); s.replace("Ö", "Oe");
    s.replace("ü", "ue"); s.replace("Ü", "Ue");
    s.replace("ß", "ss");
    return s;
}

// ── Drawing: Fahrgastinformationssystem (dot-matrix amber LED style) ──
static const int SCREEN_W  = 320;
static const int SCREEN_H  = 170;
static const int MAX_ROWS   = 3;
static const int PX_MARGIN  = 6;
static const int SEP_H      = 3;
static const int SCROLL_PX  = 2;
static const int SCROLL_GAP = 50;

// Font 1 (5x7 dot-matrix) at different textSize scales
static const int NAME_SZ = 5;  // line name: 5×7=35px visible
static const int DIR_SZ  = 2;  // direction: 2×7=14px visible
static const int CD_SZ   = 4;  // countdown: 4×7=28px visible

// Scroll state per row
static int scrollOffset[MAX_ROWS] = {0, 0, 0};
static String lastScrollText[MAX_ROWS];
static int tickerOffset = 0;  // disruption ticker horizontal scroll

// Page rotation for cycling through groups
static unsigned long lastRotateMs = 0;
static int pageOffset = 0;
// ROTATE_INTERVAL is now dynamic via cfgRotateSec

// Helper: draw glow behind text (1px amber halo for LED bleed)
void drawGlowText(TFT_eSprite& spr, int x, int y, const String& text) {
    spr.setTextColor(AMBER_DIM, BG_COLOR);
    spr.setCursor(x - 1, y); spr.print(text);
    spr.setCursor(x + 1, y); spr.print(text);
    spr.setCursor(x, y - 1); spr.print(text);
    spr.setCursor(x, y + 1); spr.print(text);
    spr.setTextColor(AMBER, BG_COLOR);
    spr.setCursor(x, y); spr.print(text);
}

void drawDisplay() {
    sprite.createSprite(SCREEN_W, SCREEN_H);
    sprite.fillSprite(BG_COLOR);
    sprite.setTextFont(1);

    // WiFi status indicator (top-right when disconnected)
    if (WiFi.status() != WL_CONNECTED) {
        sprite.setTextSize(1);
        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        sprite.setCursor(SCREEN_W - 60, 2);
        sprite.print("WiFi...");
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int totalSlots = displaySlots.size();

    if (cfgLines.empty() && cfgOebb.empty()) {
        // ── Setup screen ──
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(3);
        const char* brand = "LineTracker";
        int tw = sprite.textWidth(brand);
        sprite.setCursor((SCREEN_W - tw) / 2, 5);
        sprite.print(brand);

        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        sprite.setTextSize(1);
        const char* s1 = "Browser oeffnen:";
        tw = sprite.textWidth(s1);
        sprite.setCursor((SCREEN_W - tw) / 2, 42);
        sprite.print(s1);

        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(2);
        const char* hostname = "linetracker.local";
        tw = sprite.textWidth(hostname);
        sprite.setCursor((SCREEN_W - tw) / 2, 60);
        sprite.print(hostname);

        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        sprite.setTextSize(1);
        String ip = "oder: " + WiFi.localIP().toString();
        tw = sprite.textWidth(ip);
        sprite.setCursor((SCREEN_W - tw) / 2, 90);
        sprite.print(ip);

        sprite.drawFastHLine(60, 108, 200, AMBER_DIM);

        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(2);
        const char* s3 = "Linien auswaehlen";
        tw = sprite.textWidth(s3);
        sprite.setCursor((SCREEN_W - tw) / 2, 120);
        sprite.print(s3);
    } else if (totalSlots == 0) {
        // ── Loading screen ──
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(2);
        const char* msg = fetchError ? "Daten werden geladen..." : "Lade Abfahrten...";
        int tw = sprite.textWidth(msg);
        sprite.setCursor((SCREEN_W - tw) / 2, (SCREEN_H - 14) / 2);
        sprite.print(msg);
    } else {
        // ── Smart display with page rotation ──
        // Cycle pages when more slots than rows
        if (totalSlots > MAX_ROWS) {
            if (millis() - lastRotateMs > (unsigned long)cfgRotateSec * 1000) {
                pageOffset += MAX_ROWS;
                if (pageOffset >= totalSlots) pageOffset = 0;
                lastRotateMs = millis();
                // Reset scroll for new page
                for (int j = 0; j < MAX_ROWS; j++) {
                    scrollOffset[j] = 0;
                    lastScrollText[j] = "";
                }
            }
        } else {
            pageOffset = 0;
        }

        // Reserve bottom strip for disruption ticker if active
        bool showTicker = cfgShowDisruptions && !disruptions.empty();
        int tickerH = showTicker ? 13 : 0;
        int usableH = SCREEN_H - tickerH;

        int rows = min(totalSlots - pageOffset, MAX_ROWS);
        int totalSep = (rows - 1) * SEP_H;
        int rowH = (usableH - totalSep) / rows;

        // Visible height for centering (Font 1 char = 7px, not 8px)
        int nameH = 7 * NAME_SZ;
        int cdH   = 7 * CD_SZ;
        int dirLineH = 7 * DIR_SZ;
        int dirCharW = 6 * DIR_SZ;

        // ── Dynamic column widths ──
        sprite.setTextSize(NAME_SZ);
        int maxLeftW = 0;
        for (int i = 0; i < rows; i++) {
            int lw = sprite.textWidth(displaySlots[pageOffset + i].lineName);
            if (lw > maxLeftW) maxLeftW = lw;
        }

        sprite.setTextSize(CD_SZ);
        int maxRightW = 0;
        for (int i = 0; i < rows; i++) {
            if (displaySlots[pageOffset + i].countdown > 0) {
                int rw = sprite.textWidth(String(displaySlots[pageOffset + i].countdown));
                if (rw > maxRightW) maxRightW = rw;
            }
        }
        int arrSz = 10;
        if (maxRightW < arrSz * 2 + 2) maxRightW = arrSz * 2 + 2;

        int centerX = PX_MARGIN + maxLeftW + PX_MARGIN * 2;
        int centerW = SCREEN_W - centerX - maxRightW - PX_MARGIN * 3;
        if (centerW < 20) centerW = 20;

        for (int i = 0; i < rows; i++) {
            const Departure& d = displaySlots[pageOffset + i];
            int y = i * (rowH + SEP_H);
            int centerY = y + rowH / 2;

            // Separator
            if (i > 0) {
                sprite.fillRect(0, y - SEP_H, SCREEN_W, SEP_H, TFT_BLACK);
            }

            // ── Line name (huge, left, with glow) ──
            sprite.setTextSize(NAME_SZ);
            drawGlowText(sprite, PX_MARGIN, centerY - nameH / 2, d.lineName);

            // ── Direction (always clipped to center column) ──
            sprite.setTextSize(DIR_SZ);
            String dir = sanitize(d.towards);

            if (lastScrollText[i] != dir) {
                scrollOffset[i] = 0;
                lastScrollText[i] = dir;
            }

            {
                // Render direction in a sub-sprite that clips to centerW
                TFT_eSprite clip = TFT_eSprite(&tft);
                clip.createSprite(centerW, rowH);
                clip.fillSprite(BG_COLOR);
                clip.setTextFont(1);
                clip.setTextSize(DIR_SZ);
                clip.setTextColor(AMBER, BG_COLOR);

                int dirW = clip.textWidth(dir);
                int maxCharsPerLine = centerW / max(dirCharW, 1);

                if ((int)dir.length() <= maxCharsPerLine) {
                    // Single line — vertically centered in clip
                    clip.setCursor(0, rowH / 2 - dirLineH / 2);
                    clip.print(dir);
                } else {
                    // Try two-line split at space near middle
                    int splitIdx = -1;
                    int mid = dir.length() / 2;
                    for (int off = 0; off <= mid; off++) {
                        if (mid + off < (int)dir.length() && dir.charAt(mid + off) == ' ') {
                            splitIdx = mid + off; break;
                        }
                        if (mid - off >= 0 && dir.charAt(mid - off) == ' ') {
                            splitIdx = mid - off; break;
                        }
                    }

                    if (splitIdx > 0) {
                        String line1 = dir.substring(0, splitIdx);
                        String line2 = dir.substring(splitIdx + 1);
                        int l1W = clip.textWidth(line1);
                        int l2W = clip.textWidth(line2);
                        bool needsScroll = (l1W > centerW || l2W > centerW);

                        if (!needsScroll) {
                            // Two lines fit — render stacked
                            int twoLineH = dirLineH * 2 + 2;
                            int startY = rowH / 2 - twoLineH / 2;
                            clip.setCursor(0, startY);
                            clip.print(line1);
                            clip.setCursor(0, startY + dirLineH + 2);
                            clip.print(line2);
                        } else {
                            // Too wide even split — scroll single line
                            int sy = rowH / 2 - dirLineH / 2;
                            int sx = -scrollOffset[i];
                            clip.setCursor(sx, sy);
                            clip.print(dir);
                            clip.setCursor(sx + dirW + SCROLL_GAP, sy);
                            clip.print(dir);
                            scrollOffset[i] += SCROLL_PX;
                            if (scrollOffset[i] >= dirW + SCROLL_GAP)
                                scrollOffset[i] = 0;
                        }
                    } else {
                        // No space to split — scroll
                        int sy = rowH / 2 - dirLineH / 2;
                        int sx = -scrollOffset[i];
                        clip.setCursor(sx, sy);
                        clip.print(dir);
                        clip.setCursor(sx + dirW + SCROLL_GAP, sy);
                        clip.print(dir);
                        scrollOffset[i] += SCROLL_PX;
                        if (scrollOffset[i] >= dirW + SCROLL_GAP)
                            scrollOffset[i] = 0;
                    }
                }

                clip.pushToSprite(&sprite, centerX, y);
                clip.deleteSprite();
            }

            // ── Countdown or arriving blink (right) ──
            sprite.setTextSize(CD_SZ);
            if (d.countdown == 0) {
                bool phase = (millis() / 1000) % 2 == 0;
                int sz  = arrSz;
                int gap = 2;
                int tw  = sz * 2 + gap;
                int th  = sz * 2 + gap;
                int ax  = SCREEN_W - PX_MARGIN - tw;
                int ay  = centerY - th / 2;
                if (phase) {
                    sprite.fillRect(ax + sz + gap, ay,            sz, sz, AMBER);
                    sprite.fillRect(ax,            ay + sz + gap, sz, sz, AMBER);
                } else {
                    sprite.fillRect(ax,            ay,            sz, sz, AMBER);
                    sprite.fillRect(ax + sz + gap, ay + sz + gap, sz, sz, AMBER);
                }
            } else {
                // Main countdown
                String cdStr = String(d.countdown);
                int cdW = sprite.textWidth(cdStr);

                if (cfgShowNext) {
                    // Find next departure for same line+direction
                    int nextCd = -1;
                    bool found1 = false;
                    for (auto& dep : departures) {
                        if (dep.lineName == d.lineName && dep.towards == d.towards) {
                            if (!found1) { found1 = true; continue; }  // skip first (= current)
                            nextCd = dep.countdown; break;
                        }
                    }

                    if (nextCd >= 0) {
                        // Stack: main countdown top, "→ Xmin" below
                        int nextSz = 2;
                        int nextH  = 7 * nextSz;
                        int totalCdH = cdH + 3 + nextH;
                        int topY = centerY - totalCdH / 2;
                        drawGlowText(sprite, SCREEN_W - PX_MARGIN - cdW, topY, cdStr);
                        sprite.setTextSize(nextSz);
                        sprite.setTextColor(AMBER_DIM, BG_COLOR);
                        String nextStr = ">" + String(nextCd);
                        int nextW = sprite.textWidth(nextStr);
                        sprite.setCursor(SCREEN_W - PX_MARGIN - nextW, topY + cdH + 3);
                        sprite.print(nextStr);
                        sprite.setTextSize(CD_SZ);
                    } else {
                        drawGlowText(sprite, SCREEN_W - PX_MARGIN - cdW, centerY - cdH / 2, cdStr);
                    }
                } else {
                    drawGlowText(sprite, SCREEN_W - PX_MARGIN - cdW, centerY - cdH / 2, cdStr);
                }
            }
        }

        // ── Page indicator dots (if multiple pages, no ticker) ──
        if (totalSlots > MAX_ROWS && !showTicker) {
            int pages = (totalSlots + MAX_ROWS - 1) / MAX_ROWS;
            int currentPage = pageOffset / MAX_ROWS;
            int dotR = 2;
            int dotGap = 8;
            int dotsW = pages * (dotR * 2) + (pages - 1) * dotGap;
            int dotX = (SCREEN_W - dotsW) / 2;
            int dotY = usableH - 5;
            for (int p = 0; p < pages; p++) {
                uint16_t col = (p == currentPage) ? AMBER : AMBER_DIM;
                sprite.fillCircle(dotX + p * (dotR * 2 + dotGap) + dotR, dotY, dotR, col);
            }
        }

        // ── Disruption ticker ──
        if (showTicker) {
            int ty = usableH;
            sprite.fillRect(0, ty, SCREEN_W, tickerH, TFT_BLACK);
            sprite.drawFastHLine(0, ty, SCREEN_W, AMBER_DIM);

            // Build full ticker string from all disruptions
            String tickerText = "";
            for (size_t ti2 = 0; ti2 < disruptions.size(); ti2++) {
                if (ti2 > 0) tickerText += "  |  ";
                tickerText += disruptions[ti2];
            }
            tickerText += "     ";

            TFT_eSprite tickerClip = TFT_eSprite(&tft);
            tickerClip.createSprite(SCREEN_W, tickerH - 1);
            tickerClip.fillSprite(TFT_BLACK);
            tickerClip.setTextFont(1);
            tickerClip.setTextSize(1);
            tickerClip.setTextColor(AMBER_DIM, TFT_BLACK);
            int textW = tickerClip.textWidth(tickerText);
            tickerClip.setCursor(-tickerOffset, (tickerH - 1 - 7) / 2);
            tickerClip.print(tickerText);
            // Wrap-around copy
            tickerClip.setCursor(-tickerOffset + textW, (tickerH - 1 - 7) / 2);
            tickerClip.print(tickerText);
            tickerClip.pushToSprite(&sprite, 0, ty + 1);
            tickerClip.deleteSprite();

            tickerOffset += 1;
            if (tickerOffset >= textW) tickerOffset = 0;
        }
    }
    xSemaphoreGive(dataMutex);

    sprite.pushSprite(0, 0);
    sprite.deleteSprite();
}

// ── Night mode backlight control ─────────────────────────────────────
bool lastNightState = false;

void applyNightMode() {
    if (cfgNightFrom < 0 || cfgNightTo < 0) {
        // Night mode disabled — ensure day brightness is applied
        if (lastNightState) {
            ledcWrite(0, cfgBrightness);
            lastNightState = false;
        }
        return;
    }
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) return;
    int h = timeinfo.tm_hour;

    bool isNight;
    if (cfgNightFrom <= cfgNightTo) {
        isNight = (h >= cfgNightFrom && h < cfgNightTo);
    } else {
        // Wraps midnight, e.g. 22–07
        isNight = (h >= cfgNightFrom || h < cfgNightTo);
    }

    if (isNight != lastNightState) {
        ledcWrite(0, isNight ? cfgNightBright : cfgBrightness);
        lastNightState = isNight;
    }
}

// ── FreeRTOS tasks ───────────────────────────────────────────────────
void dataTask(void* param) {
    unsigned long lastCacheRefresh = millis();
    unsigned long lastOtaCheck = millis();
    for (;;) {
        // Auto-reconnect WiFi if disconnected
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi lost, reconnecting...");
            WiFi.disconnect();
            WiFi.begin();  // uses stored NVS credentials
            int retries = 0;
            while (WiFi.status() != WL_CONNECTED && retries < 30) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                retries++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("WiFi reconnected: " + WiFi.localIP().toString());
                MDNS.begin("linetracker");
                MDNS.addService("http", "tcp", 80);
            } else {
                Serial.println("WiFi reconnect failed, will retry next cycle");
            }
        }
        fetchDepartures();
        fetchOebbDepartures();
        // Refresh CSV cache every 24h
        if (millis() - lastCacheRefresh > CACHE_MAX_AGE_MS) {
            refreshCsvCache(true);
            lastCacheRefresh = millis();
        }
        // Check for OTA updates every 6h
        if (millis() - lastOtaCheck > OTA_CHECK_INTERVAL_MS) {
            checkOtaUpdate();
            lastOtaCheck = millis();
        }
        // Wait 20s, but wake early if config changed
        for (int t = 0; t < 40; t++) {  // 40 × 500ms = 20s
            if (configChanged) {
                configChanged = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void displayTask(void* param) {
    unsigned long lastNightCheck = 0;
    for (;;) {
        drawDisplay();
        if (millis() - lastNightCheck > 10000) {
            applyNightMode();
            lastNightCheck = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // ~20fps for smooth scrolling
    }
}

// ── Reset button ─────────────────────────────────────────────────────
// BOOT 3s = WiFi reset (open portal, keep lines)
// BOOT 10s = Factory reset (erase everything)
void checkResetButton() {
    static unsigned long pressStart = 0;
    static bool actionTaken = false;
    if (digitalRead(0) == LOW) {
        if (pressStart == 0) { pressStart = millis(); actionTaken = false; }
        unsigned long held = millis() - pressStart;
        if (held > 10000 && !actionTaken) {
            actionTaken = true;
            tft.fillScreen(TFT_RED);
            tft.setTextColor(TFT_WHITE);
            tft.setTextFont(1);
            tft.setTextSize(2);
            tft.setCursor(10, 70);
            tft.print("Factory Reset...");
            SPIFFS.remove(CONFIG_PATH);
            WiFiManager wm;
            wm.resetSettings();
            delay(500);
            ESP.restart();
        } else if (held > 5000) {
            tft.fillScreen(TFT_RED);
            tft.setTextColor(TFT_WHITE);
            tft.setTextFont(1);
            tft.setTextSize(2);
            tft.setCursor(10, 60);
            tft.print("  Halten: Reset");
            tft.setCursor(10, 90);
            tft.print("  Loslassen: WiFi");
        } else if (held > 3000) {
            tft.fillScreen(BG_COLOR);
            tft.setTextColor(AMBER, BG_COLOR);
            tft.setTextFont(1);
            tft.setTextSize(2);
            tft.setCursor(10, 60);
            tft.print("  Loslassen: WiFi Reset");
            tft.setCursor(10, 90);
            tft.print("  Halten: Factory Reset");
        }
    } else {
        if (pressStart > 0 && !actionTaken) {
            unsigned long held = millis() - pressStart;
            if (held > 3000) {
                // WiFi reset — keep line config, open portal
                File f = SPIFFS.open(WIFI_RESET_FLAG, "w");
                if (f) { f.print("1"); f.close(); }
                ESP.restart();
            }
        }
        pressStart = 0;
        actionTaken = false;
    }
}

// ── Setup & Loop ─────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("LineTracker v" + String(FW_VERSION) + " starting...");

    tft.init();
    delay(150);  // give ST7789 time to wake up before first draw
    tft.setRotation(1);

    // Fahrgastinformationssystem colors
    BG_COLOR  = tft.color565(10, 8, 5);
    AMBER     = tft.color565(255, 191, 0);
    AMBER_DIM = tft.color565(50, 38, 0);

    tft.fillScreen(BG_COLOR);
    tft.setTextColor(AMBER);

    // Backlight via LEDC PWM — brightness set after loadConfig()
    ledcSetup(0, 2000, 8);
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, 255);  // start at full, updated after config loads

    // ── Splash screen ───────────────────────────────────────────────
    tft.setTextFont(1);
    tft.setTextColor(AMBER, BG_COLOR);

    // "LineTracker" centered, large
    tft.setTextSize(4);
    const char* brand = "LineTracker";
    int bw = tft.textWidth(brand);
    tft.setCursor((320 - bw) / 2, 30);
    tft.print(brand);

    // Decorative line
    int lineY = 75;
    tft.drawFastHLine(40, lineY, 240, AMBER_DIM);

    // "by Leo Blum"
    tft.setTextSize(2);
    tft.setTextColor(AMBER_DIM, BG_COLOR);
    const char* author = "by Leo Blum";
    int aw = tft.textWidth(author);
    tft.setCursor((320 - aw) / 2, 85);
    tft.print(author);

    // Version
    tft.setTextSize(1);
    String verStr = "v" + String(FW_VERSION);
    int vw = tft.textWidth(verStr);
    tft.setCursor((320 - vw) / 2, 112);
    tft.print(verStr);

    // Data attribution
    tft.setTextColor(tft.color565(30, 24, 5), BG_COLOR);
    const char* attr = "Data: Stadt Wien (CC BY 4.0) / OeBB";
    int atw = tft.textWidth(attr);
    tft.setCursor((320 - atw) / 2, 155);
    tft.print(attr);

    delay(2000);  // show splash for 2 seconds
    // ── End splash ──────────────────────────────────────────────────

    if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");

    loadConfig();
    ledcWrite(0, cfgBrightness);

    // Show loading status below splash
    tft.setTextColor(AMBER_DIM, BG_COLOR);
    tft.setTextSize(1);
    tft.setCursor((320 - tft.textWidth("Verbinde WiFi...")) / 2, 140);
    tft.print("Verbinde WiFi...");

    setupWiFi();

    tft.fillRect(0, 135, 320, 20, BG_COLOR);
    tft.setCursor((320 - tft.textWidth("Synchronisiere Zeit...")) / 2, 140);
    tft.print("Synchronisiere Zeit...");

    // NTP time sync
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
    {
        struct tm ti;
        int ntpWaits = 0;
        while (!getLocalTime(&ti, 1000) && ntpWaits < 8) ntpWaits++;
        Serial.println(getLocalTime(&ti, 0) ? "NTP synced" : "NTP timeout, will retry");
    }

    tft.fillRect(0, 135, 320, 20, BG_COLOR);
    tft.setCursor((320 - tft.textWidth("Lade Stationsdaten...")) / 2, 140);
    tft.print("Lade Stationsdaten...");
    refreshCsvCache();

    if (MDNS.begin("linetracker")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: linetracker.local");
    }

    Serial.println("Connected! IP: " + WiFi.localIP().toString());
    Serial.print("WL Lines: "); Serial.println(cfgLines.size());
    Serial.print("OeBB Stations: "); Serial.println(cfgOebb.size());

    startConfigServer();

    dataMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(dataTask,    "data",    16384, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(displayTask, "display", 8192,  NULL, 1, NULL, 1);
}

void loop() {
    server.handleClient();
    checkResetButton();
    delay(5);
}
