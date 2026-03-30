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
#include <DNSServer.h>
#include <WiFiManager.h>
#include <HTTPUpdate.h>
#include <vector>
#include <map>
#include <algorithm>
#include <time.h>

// ── In-memory log ring buffer (accessible via /logs) ─────────────────
#define LOG_BUF_SIZE 8192
static char logBuf[LOG_BUF_SIZE];
static int  logHead = 0;
static bool logWrapped = false;

void logWrite(const char* msg) {
    Serial.print(msg);
    for (int i = 0; msg[i]; i++) {
        logBuf[logHead] = msg[i];
        logHead = (logHead + 1) % LOG_BUF_SIZE;
        if (logHead == 0) logWrapped = true;
    }
}

void logf(const char* fmt, ...) {
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    logWrite(tmp);
}

String getLogContents() {
    String out;
    out.reserve(LOG_BUF_SIZE);
    if (logWrapped) for (int i = logHead; i < LOG_BUF_SIZE; i++) if (logBuf[i]) out += logBuf[i];
    for (int i = 0; i < logHead; i++) if (logBuf[i]) out += logBuf[i];
    return out;
}

// ── Firmware version ────────────────────────────────────────────────
#define FW_VERSION "1.1.7"
#define OTA_VERSION_URL "https://raw.githubusercontent.com/blumleo2004/linetracker/master/version.json"
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

// ── Struct for discovered lines at a station ─────────────────────────
struct FoundLine {
    String rbl;
    String lineName;
    String towards;
    String type;
    String stopName;
};

// Direction cache: RBL → line name, towards, type
static const char* DIR_CACHE_PATH = "/dir_cache.json";
static std::map<String, std::vector<FoundLine>> dirCache;  // rbl → all lines (multi-line RBLs)

// Static line directions: linienId → {name, type, terminusH, terminusR}
static const char* LINE_DIRS_PATH = "/line_dirs.json";
struct LineDirInfo {
    String name;      // line designation (e.g. "10A", "U6")
    String type;      // transport type (e.g. "ptTram", "ptMetro")
    String terminusH; // end station direction H
    String terminusR; // end station direction R
};
static std::map<String, LineDirInfo> lineDirMap;  // linienId → info

// Steige CSV entry (defined here so search index types are available globally)
struct SteigeInfo {
    String rbl;
    String linienId;
    String richtung;  // "H" or "R"
};

// PSRAM-backed search indexes — char arrays keep small allocations out of internal heap
struct HaltRecord   { char haltId[12]; char name[64]; };
struct SteigeRecord { char haltId[12]; char rbl[8]; char linienId[12]; char richtung; char _pad[3]; };
static HaltRecord*   haltRecords        = nullptr;
static int           haltRecordCount    = 0;
static SteigeRecord* steigeRecords      = nullptr;
static int           steigeRecordCount  = 0;

// WiFi portal state
static WiFiManager   wm;
static DNSServer     apDns;
static unsigned long wifiDownSince  = 0;
static volatile bool portalOpen     = false;
static volatile bool portalShouldOpen = false;

void loadDirCache() {
    if (!SPIFFS.exists(DIR_CACHE_PATH)) return;
    File f = SPIFFS.open(DIR_CACHE_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();
    for (JsonPair kv : doc.as<JsonObject>()) {
        String rbl = kv.key().c_str();
        for (JsonObject obj : kv.value().as<JsonArray>()) {
            FoundLine fl;
            fl.rbl = rbl; fl.lineName = obj["n"] | ""; fl.towards = obj["t"] | "";
            fl.type = obj["y"] | ""; fl.stopName = obj["s"] | "";
            if (fl.lineName.length() > 0) dirCache[rbl].push_back(fl);
        }
    }
    int total = 0; for (auto& kv2 : dirCache) total += kv2.second.size();
    Serial.printf("Direction cache loaded: %d RBLs, %d lines\n", dirCache.size(), total);
}

void saveDirCache() {
    JsonDocument doc;
    for (auto& pair : dirCache) {
        JsonArray arr = doc[pair.first].to<JsonArray>();
        for (auto& fl : pair.second) {
            JsonObject obj = arr.add<JsonObject>();
            obj["n"] = fl.lineName; obj["t"] = fl.towards;
            obj["y"] = fl.type;    obj["s"] = fl.stopName;
        }
    }
    File f = SPIFFS.open(DIR_CACHE_PATH, "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}

void cacheDirEntry(const String& rbl, const String& name, const String& towards, const String& type, const String& stop) {
    auto& vec = dirCache[rbl];
    for (auto& fl : vec) { if (fl.lineName == name && fl.towards == towards) return; }
    FoundLine fl;
    fl.rbl = rbl; fl.lineName = name; fl.towards = towards; fl.type = type; fl.stopName = stop;
    vec.push_back(fl);
}

void loadLineDirections() {
    lineDirMap.clear();
    if (!SPIFFS.exists(LINE_DIRS_PATH)) return;
    File f = SPIFFS.open(LINE_DIRS_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();
    for (JsonPair kv : doc.as<JsonObject>()) {
        LineDirInfo info;
        info.name      = kv.value()["n"] | "";
        info.type      = kv.value()["y"] | "";
        info.terminusH = kv.value()["h"] | "";
        info.terminusR = kv.value()["r"] | "";
        if (info.name.length() > 0) {
            lineDirMap[String(kv.key().c_str())] = info;
        }
    }
    logf("Line directions loaded: %d entries\n", lineDirMap.size());
}

static int  cfgRotateSec      = 5;     // page switch interval in seconds (default 5)
static int  cfgBrightness     = 255;   // backlight 0-255 (default max)
static int  cfgNightFrom      = -1;    // night mode start hour (0-23), -1 = disabled
static int  cfgNightTo        = -1;    // night mode end hour (0-23)
static int  cfgNightBright    = 20;    // backlight during night mode
static bool   cfgShowNext        = false; // show next departure below main countdown
static bool   cfgShowDisruptions = false; // show WL disruption ticker at bottom
static String cfgHostname        = "";    // mDNS hostname, generated from MAC on first boot

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
    cfgHostname        = doc["hostname"]          | "";
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
    doc["hostname"]         = cfgHostname;
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
static volatile bool otaInProgress = false;
static volatile int  otaPercent    = 0;
static String        otaNewVersion = "";

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
    time_t cached = (time_t)strtoull(ts.c_str(), NULL, 10);
    if (cached < 1700000000) return false;  // invalid or legacy millis() timestamp
    time_t now;
    time(&now);
    if (now < 1700000000) return true;  // NTP not synced yet, trust existing cache files
    return difftime(now, cached) < (CACHE_MAX_AGE_MS / 1000);
}

void saveCacheTimestamp() {
    time_t now;
    time(&now);
    File f = SPIFFS.open(CACHE_TS_PATH, "w");
    if (f) { f.print((unsigned long long)now); f.close(); }
}

void buildLineDirections() {
    logf("Building line directions... heap=%u psram=%u\n",
         ESP.getFreeHeap(), ESP.getFreePsram());

    // Step 1: Load all line info from linien.csv
    std::map<String, std::pair<String,String>> lineInfoMap; // id → {name, type}
    {
        File f = SPIFFS.open(CACHE_LINIEN_PATH, "r");
        if (!f) { logf("linien.csv not found\n"); return; }
        while (f.available()) {
            String line = f.readStringUntil('\n');
            int s1 = line.indexOf(';'); if (s1 < 0) continue;
            String id = line.substring(0, s1); id.replace("\"", "");
            int s2 = line.indexOf(';', s1 + 1); if (s2 < 0) continue;
            String name = line.substring(s1 + 1, s2); name.replace("\"", "");
            int s3 = line.indexOf(';', s2 + 1); if (s3 < 0) continue;
            int s4 = line.indexOf(';', s3 + 1); if (s4 < 0) continue;
            int s5 = line.indexOf(';', s4 + 1);
            String type = (s5 > 0) ? line.substring(s4 + 1, s5) : line.substring(s4 + 1);
            type.replace("\"", ""); type.trim();
            if (name.length() > 0) lineInfoMap[id] = {name, type};
        }
        f.close();
    }

    // Step 2: Scan steige.csv for terminus stations (highest REIHENFOLGE per line+direction)
    struct TermInfo { int maxH = -1; String haltH; int maxR = -1; String haltR; };
    std::map<String, TermInfo> termMap;
    {
        const int MAX_STEIGE = 10000;
        if (steigeRecords) { free(steigeRecords); steigeRecords = nullptr; steigeRecordCount = 0; }
        steigeRecords = (SteigeRecord*)ps_malloc(MAX_STEIGE * sizeof(SteigeRecord));
        if (!steigeRecords) logf("[buildLD] WARN: ps_malloc steigeRecords failed\n");

        File f = SPIFFS.open(CACHE_STEIGE_PATH, "r");
        if (!f) { logf("steige.csv not found\n"); return; }
        int yieldCounter = 0;
        while (f.available()) {
            if (++yieldCounter % 200 == 0) vTaskDelay(pdMS_TO_TICKS(1));
            String line = f.readStringUntil('\n');
            int s1 = line.indexOf(';'); if (s1 < 0) continue;
            int s2 = line.indexOf(';', s1 + 1); if (s2 < 0) continue;
            int s3 = line.indexOf(';', s2 + 1); if (s3 < 0) continue;
            int s4 = line.indexOf(';', s3 + 1); if (s4 < 0) continue;
            int s5 = line.indexOf(';', s4 + 1); if (s5 < 0) continue;
            String linienId = line.substring(s1 + 1, s2); linienId.replace("\"", "");
            String haltId   = line.substring(s2 + 1, s3); haltId.replace("\"", "");
            String richtung = line.substring(s3 + 1, s4); richtung.replace("\"", "");
            String seqStr   = line.substring(s4 + 1, s5); seqStr.replace("\"", "");
            int seq = seqStr.toInt();
            auto& ti = termMap[linienId];
            if (richtung == "H" && seq > ti.maxH) { ti.maxH = seq; ti.haltH = haltId; }
            else if (richtung == "R" && seq > ti.maxR) { ti.maxR = seq; ti.haltR = haltId; }
            // Fill steigeRecords (deduplicated via sort+unique after scan)
            int s6 = line.indexOf(';', s5 + 1);
            if (s6 >= 0 && steigeRecords && steigeRecordCount < MAX_STEIGE) {
                String rbl = line.substring(s5 + 1, s6); rbl.replace("\"", ""); rbl.trim();
                if (rbl.length() > 0) {
                    auto& r = steigeRecords[steigeRecordCount++];
                    strncpy(r.haltId,   haltId.c_str(),   11); r.haltId[11]   = '\0';
                    strncpy(r.rbl,      rbl.c_str(),        7); r.rbl[7]       = '\0';
                    strncpy(r.linienId, linienId.c_str(),  11); r.linienId[11] = '\0';
                    r.richtung = (richtung == "H") ? 'H' : 'R';
                }
            }
        }
        f.close();
        if (steigeRecords && steigeRecordCount > 0) {
            std::sort(steigeRecords, steigeRecords + steigeRecordCount, [](const SteigeRecord& a, const SteigeRecord& b) {
                int c = strcmp(a.haltId, b.haltId); if (c != 0) return c < 0;
                c = strcmp(a.rbl, b.rbl); if (c != 0) return c < 0;
                return strcmp(a.linienId, b.linienId) < 0;
            });
            auto endIt = std::unique(steigeRecords, steigeRecords + steigeRecordCount, [](const SteigeRecord& a, const SteigeRecord& b) {
                return strcmp(a.haltId, b.haltId) == 0 && strcmp(a.rbl, b.rbl) == 0 && strcmp(a.linienId, b.linienId) == 0;
            });
            steigeRecordCount = endIt - steigeRecords;
        }
        logf("[buildLD] steige scan done: %d records, heap=%u psram=%u\n",
             steigeRecordCount, ESP.getFreeHeap(), ESP.getFreePsram());
    }

    // Step 3: Scan full halt.csv — resolve terminus names AND build search index
    std::map<String, String> haltNames;
    for (auto& p : termMap) {
        if (p.second.haltH.length() > 0) haltNames[p.second.haltH] = "";
        if (p.second.haltR.length() > 0) haltNames[p.second.haltR] = "";
    }
    const int MAX_HALT = 2200;
    if (haltRecords) { free(haltRecords); haltRecords = nullptr; haltRecordCount = 0; }
    haltRecords = (HaltRecord*)ps_malloc(MAX_HALT * sizeof(HaltRecord));
    if (!haltRecords) logf("[buildLD] WARN: ps_malloc haltRecords failed\n");
    {
        File f = SPIFFS.open(CACHE_HALT_PATH, "r");
        if (!f) { logf("halt.csv not found\n"); return; }
        while (f.available()) {
            String line = f.readStringUntil('\n');
            int s1 = line.indexOf(';'); if (s1 < 0) continue;
            String haltId = line.substring(0, s1); haltId.replace("\"", "");
            int s2 = line.indexOf(';', s1 + 1); if (s2 < 0) continue;
            int s3 = line.indexOf(';', s2 + 1); if (s3 < 0) continue;
            int s4 = line.indexOf(';', s3 + 1); if (s4 < 0) continue;
            String name = line.substring(s3 + 1, s4); name.replace("\"", "");
            if (name.length() == 0) continue;
            auto it = haltNames.find(haltId);
            if (it != haltNames.end()) it->second = name;
            if (haltRecords && haltRecordCount < MAX_HALT) {
                auto& r = haltRecords[haltRecordCount++];
                strncpy(r.haltId, haltId.c_str(), 11); r.haltId[11] = '\0';
                strncpy(r.name,   name.c_str(),   63); r.name[63]   = '\0';
            }
        }
        f.close();
    }

    // Step 4: Build and save JSON
    JsonDocument doc;
    int count = 0;
    for (auto& p : termMap) {
        auto li = lineInfoMap.find(p.first);
        if (li == lineInfoMap.end()) continue;
        JsonObject obj = doc[p.first].to<JsonObject>();
        obj["n"] = li->second.first;
        obj["y"] = li->second.second;
        obj["h"] = haltNames[p.second.haltH];
        obj["r"] = haltNames[p.second.haltR];
        count++;
    }
    {
        File f = SPIFFS.open(LINE_DIRS_PATH, "w");
        if (f) { serializeJson(doc, f); f.close(); }
    }
    logf("Line directions built: %d lines, halt index: %d, steige index: %d, heap=%u psram=%u\n",
         count, haltRecordCount, steigeRecordCount, ESP.getFreeHeap(), ESP.getFreePsram());

    // Reload into memory
    loadLineDirections();
    logf("lineDirMap loaded: %d entries\n", lineDirMap.size());
}

bool refreshCsvCache(bool force = false) {
    if (!force && isCacheValid()
        && SPIFFS.exists(CACHE_HALT_PATH)
        && SPIFFS.exists(CACHE_STEIGE_PATH)
        && SPIFFS.exists(CACHE_LINIEN_PATH)) {
        logf("CSV cache valid, skipping download\n");
        return true;
    }
    logf("Downloading CSV cache...\n");
    bool ok = true;
    ok &= downloadCsvToCache("https://data.wien.gv.at/csv/wienerlinien-ogd-haltestellen.csv", CACHE_HALT_PATH);
    ok &= downloadCsvToCache("https://data.wien.gv.at/csv/wienerlinien-ogd-steige.csv", CACHE_STEIGE_PATH);
    ok &= downloadCsvToCache("https://data.wien.gv.at/csv/wienerlinien-ogd-linien.csv", CACHE_LINIEN_PATH);
    if (ok) {
        saveCacheTimestamp();
        logf("CSV cache updated\n");
    } else {
        logf("CSV cache download failed (partial)\n");
    }
    return ok;
}

// Look up line name and type from linien cache by LINIEN_ID
// Linien CSV: "LINIEN_ID";"BEZEICHNUNG";"REIHENFOLGE";"ECHTZEIT";"VERKEHRSMITTEL";"STAND"
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
        // BEZEICHNUNG (field 2)
        int s2 = line.indexOf(';', s1 + 1);
        if (s2 < 0) { f.close(); return false; }
        outName = line.substring(s1 + 1, s2);
        outName.replace("\"", "");
        // skip REIHENFOLGE (field 3)
        int s3 = line.indexOf(';', s2 + 1);
        if (s3 < 0) { f.close(); return false; }
        // skip ECHTZEIT (field 4)
        int s4 = line.indexOf(';', s3 + 1);
        if (s4 < 0) { f.close(); return false; }
        // VERKEHRSMITTEL (field 5)
        int s5 = line.indexOf(';', s4 + 1);
        String vm = (s5 > 0) ? line.substring(s4 + 1, s5) : line.substring(s4 + 1);
        vm.replace("\"", "");
        vm.trim();
        outType = vm;
        f.close();
        return true;
    }
    f.close();
    return false;
}

// Normalize string for search: Umlauts to ASCII, lowercase
String normalizeForSearch(const String& s) {
    String r = s;
    r.toLowerCase();
    r.replace("ä", "ae"); r.replace("ö", "oe");
    r.replace("ü", "ue"); r.replace("ß", "ss");
    return r;
}

// Transport type priority for sorting search results
int transportPriority(const String& type) {
    if (type == "ptMetro") return 0;
    if (type == "ptTram" || type == "ptTramWLB") return 1;
    if (type.startsWith("ptBus")) return 2;
    if (type == "ptTrainS") return 3;
    return 4;
}

// Search stations from cached haltestellen CSV (Umlaut-tolerant)
std::vector<std::pair<String,String>> searchStations(const String& query) {
    std::vector<std::pair<String,String>> results;

    String lowerQuery = query;
    lowerQuery.toLowerCase();
    String normQuery = normalizeForSearch(query);

    // Fast path: use pre-built PSRAM index (built during buildLineDirections)
    if (haltRecords && haltRecordCount > 0) {
        for (int i = 0; i < haltRecordCount; i++) {
            String name = String(haltRecords[i].name);
            String lowerName = name; lowerName.toLowerCase();
            String normName = normalizeForSearch(name);
            if (lowerName.indexOf(lowerQuery) >= 0 || normName.indexOf(normQuery) >= 0) {
                results.push_back({String(haltRecords[i].haltId), name});
                if (results.size() >= 15) break;
            }
        }
        return results;
    }

    // Slow fallback: scan halt.csv (used before buildLineDirections completes)
    File f = SPIFFS.open(CACHE_HALT_PATH, "r");
    if (!f) return results;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        // CSV: "HALTESTELLEN_ID";"TYP";"DIVA";"NAME";"GEMEINDE";...
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

        // Match against both lowercase UTF-8 and normalized ASCII
        String lowerName = name;
        lowerName.toLowerCase();
        String normName = normalizeForSearch(name);

        if (lowerName.indexOf(lowerQuery) >= 0 || normName.indexOf(normQuery) >= 0) {
            String haltId = line.substring(0, s1);
            haltId.replace("\"", "");
            if (haltId.length() > 0) {
                results.push_back({haltId, name});
            }
        }
        if (results.size() >= 15) break;
    }
    f.close();
    return results;
}

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
            for (auto& r : results) { if (r.rbl == rbl && r.linienId == linienId) { dup = true; break; } }
            if (!dup) results.push_back({rbl, linienId, richtung});
        }
    }
    f.close();
    return results;
}

// Read steige.csv once for multiple station IDs simultaneously
std::map<String, std::vector<SteigeInfo>> findSteigeForStations(const std::vector<String>& haltIds) {
    std::map<String, std::vector<SteigeInfo>> results;
    // Fast path: PSRAM sorted array + binary search
    if (steigeRecords && steigeRecordCount > 0) {
        for (auto& id : haltIds) {
            SteigeRecord key; strncpy(key.haltId, id.c_str(), 11); key.haltId[11] = '\0';
            auto lo = std::lower_bound(steigeRecords, steigeRecords + steigeRecordCount, key,
                [](const SteigeRecord& a, const SteigeRecord& b) { return strcmp(a.haltId, b.haltId) < 0; });
            auto& vec = results[id];
            while (lo < steigeRecords + steigeRecordCount && strcmp(lo->haltId, key.haltId) == 0) {
                vec.push_back({String(lo->rbl), String(lo->linienId), lo->richtung == 'H' ? "H" : "R"});
                ++lo;
            }
        }
        return results;
    }

    // Slow fallback: scan steige.csv
    unsigned long t0 = millis();
    File f = SPIFFS.open(CACHE_STEIGE_PATH, "r");
    if (!f) return results;
    int linesRead = 0, linesMatched = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        linesRead++;
        // Quick pre-filter: skip lines that don't contain any of the halt IDs
        bool preMatch = false;
        for (auto& id : haltIds) { if (line.indexOf(id) >= 0) { preMatch = true; break; } }
        if (!preMatch) continue;
        int s1 = line.indexOf(';'); if (s1 < 0) continue;
        int s2 = line.indexOf(';', s1 + 1); if (s2 < 0) continue;
        int s3 = line.indexOf(';', s2 + 1); if (s3 < 0) continue;
        String foundHalt = line.substring(s2 + 1, s3);
        foundHalt.replace("\"", "");
        bool matched = false;
        for (auto& id : haltIds) { if (foundHalt == id) { matched = true; break; } }
        if (!matched) continue;
        linesMatched++;
        int s4 = line.indexOf(';', s3 + 1); if (s4 < 0) continue;
        String richtung = line.substring(s3 + 1, s4); richtung.replace("\"", "");
        int s5 = line.indexOf(';', s4 + 1); if (s5 < 0) continue;
        int s6 = line.indexOf(';', s5 + 1); if (s6 < 0) continue;
        String rbl = line.substring(s5 + 1, s6); rbl.replace("\"", ""); rbl.trim();
        String linienId = line.substring(s1 + 1, s2); linienId.replace("\"", "");
        if (rbl.length() > 0) {
            auto& vec = results[foundHalt];
            bool dup = false;
            for (auto& r : vec) { if (r.rbl == rbl && r.linienId == linienId) { dup = true; break; } }
            if (!dup) vec.push_back({rbl, linienId, richtung});
        }
    }
    f.close();
    logf("[steige] %lums, %d lines read, %d matched\n", millis()-t0, linesRead, linesMatched);
    return results;
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
        http.setTimeout(12000);
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
                if (!dup) {
                    results.push_back(fl);
                    cacheDirEntry(fl.rbl, fl.lineName, fl.towards, fl.type, fl.stopName);
                }
            }
        }
    }
    if (!results.empty()) saveDirCache();
    return results;
}

// ── HTML Templates ───────────────────────────────────────────────────
const char HTML_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>
<title>LineTracker</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Helvetica Neue',sans-serif;background:#0a0a16;color:#e8e8f0;padding:0 16px 24px;max-width:520px;margin:0 auto;-webkit-font-smoothing:antialiased;line-height:1.5}
header{text-align:center;padding:24px 0 16px}
header h1{color:#ffbf00;font-size:1.7em;letter-spacing:1.5px;margin:0;font-weight:700}
header p{color:#555;font-size:11px;margin-top:4px;letter-spacing:.5px}
h2{color:#ffbf00;font-size:.85em;margin:16px 0 12px;letter-spacing:1px;text-transform:uppercase;font-weight:600}
.card{background:#12122a;border:1px solid rgba(255,255,255,.06);border-radius:16px;padding:18px;margin-bottom:16px;box-shadow:0 4px 20px rgba(0,0,0,.4)}
input[type=text],input[type=number]{width:100%;padding:14px 16px;border-radius:12px;border:1px solid rgba(255,255,255,.1);background:#0a0a1e;color:#fff;font-size:16px;transition:border .2s,box-shadow .2s}
input[type=text]:focus,input[type=number]:focus{outline:none;border-color:#ffbf00;box-shadow:0 0 0 3px rgba(255,191,0,.15)}
input[type=number]{width:70px;padding:10px;text-align:center}
input[type=range]{width:100%;margin:10px 0;accent-color:#ffbf00;height:6px;cursor:pointer}
button{background:#e94560;color:#fff;border:none;padding:14px 24px;border-radius:12px;font-size:15px;font-weight:600;cursor:pointer;width:100%;margin-top:10px;transition:transform .1s,opacity .15s;-webkit-tap-highlight-color:transparent}
button:hover{opacity:.9;transform:translateY(-1px)}
button:active{transform:scale(.98);opacity:.8}
button.add{background:#1a8f3c}
.line-item{display:flex;align-items:center;gap:12px;padding:14px 10px;border-bottom:1px solid rgba(255,255,255,.04);transition:background .15s}
.line-item:last-child{border-bottom:none}
.badge{display:inline-flex;align-items:center;justify-content:center;padding:6px 14px;border-radius:8px;color:#fff;font-weight:700;min-width:48px;text-align:center;font-size:14px;letter-spacing:.5px;flex-shrink:0}
.badge.metro{background:#c9935a}
.badge.tram{background:#e94560}
.badge.bus{background:#2d7cd6}
.badge.train{background:#388e3c}
.badge.unknown{background:#555}
.dir{flex:1;font-size:14px;line-height:1.4;min-width:0}
.stop{font-size:12px;color:#666;margin-top:3px}
input[type=checkbox]{width:22px;height:22px;accent-color:#ffbf00;cursor:pointer;flex-shrink:0}
.status{text-align:center;color:#666;padding:24px 8px;font-size:14px;line-height:1.6}
.rm{background:#c0392b;width:38px;min-width:38px;height:38px;padding:0;font-size:18px;margin:0;border-radius:50%;display:inline-flex;align-items:center;justify-content:center;flex-shrink:0}
.msg{background:#1a8f3c;border-radius:12px;padding:14px;margin:10px 0;text-align:center;font-weight:600}
.letters{display:flex;flex-wrap:wrap;gap:6px;justify-content:center;margin:12px 0}
.letters a{display:inline-flex;align-items:center;justify-content:center;width:36px;height:36px;background:#12122a;color:#ffbf00;border-radius:8px;text-decoration:none;font-weight:700;font-size:14px;border:1px solid rgba(255,255,255,.06);transition:all .15s;-webkit-tap-highlight-color:transparent}
.letters a:hover,.letters a.act{background:#ffbf00;color:#0a0a16;border-color:#ffbf00}
.stn{display:block;padding:13px 10px;border-bottom:1px solid rgba(255,255,255,.04);color:#e8e8f0;text-decoration:none;font-size:15px;transition:all .15s;border-radius:8px}
.stn:hover{background:rgba(255,191,0,.05);padding-left:16px}
label{display:block}
.setting{margin-top:14px}
.setting-label{font-size:13px;color:#888;display:flex;align-items:center;justify-content:space-between}
.setting-label b{color:#e8e8f0}
.btn-secondary{background:#1e1e3a;color:#ccc;font-weight:500}
.btn-secondary:hover{background:#282848}
.btn-warn{background:#e67e22}
.btn-danger{background:#c0392b}
.btn-info{background:#1a5276}
.info-text{font-size:13px;color:#888;margin-bottom:10px}
.info-text b{color:#e8e8f0}
.hint{font-size:12px;color:#555;margin-bottom:10px}
.check-label{font-size:13px;color:#888;display:flex;align-items:center;gap:8px}
.check-label input{width:auto;flex-shrink:0}
.footer{text-align:center;margin:24px 0 12px;font-size:11px;color:#444;line-height:1.8}
.footer b{color:#ffbf00}
.footer a{color:#555}
.time-row{display:flex;gap:8px;margin:10px 0;font-size:13px;color:#888;align-items:center}
.options-col{margin-top:16px;display:flex;flex-direction:column;gap:10px}
.stn-count{color:#888;font-size:12px;margin:4px 0}
@media(max-width:380px){
body{padding:0 10px 20px}
.card{padding:14px;border-radius:12px}
button{padding:12px 16px;font-size:14px}
.badge{padding:5px 10px;font-size:12px;min-width:40px}
.line-item{gap:8px;padding:12px 6px}
.letters a{width:32px;height:32px;font-size:12px}
header h1{font-size:1.4em}
}
@media(min-width:600px){
body{padding:0 24px 32px}
.card{padding:22px}
.line-item{padding:16px 12px}
}
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

void sendHtml(const String& html) {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(200, "text/html; charset=utf-8", html);
}

void handleRoot() {
    String html = FPSTR(HTML_HEAD);


    if (server.hasArg("saved")) {
        html += "<div class='msg'>Linien gespeichert!</div>";
    }

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

    html += "<div class='card'><h2>S-Bahn / Züge (ÖBB)</h2>";
    if (cfgOebb.empty()) {
        html += "<p class='status'>Noch keine ÖBB-Linien konfiguriert.</p>";
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
    html += "<button class='add' type='submit'>ÖBB-Station suchen</button>";
    html += "</form></div>";

    html += "<div class='card'><h2>Station suchen</h2>";
    html += "<form action='/search' method='GET'>";
    html += "<input type='text' name='q' placeholder='z.B. Kutschkergasse, Volksoper...' autofocus>";
    html += "<button type='submit'>Suchen</button>";
    html += "</form>";
    html += "<a href='/browse'><button class='btn-info' style='margin-top:8px'>Alle Stationen durchblättern</button></a>";
    html += "</div>";

    html += "<div class='card'><h2>Einstellungen</h2>";
    html += "<form action='/settings' method='POST'>";

    html += "<div class='setting'><div class='setting-label'>Seitenwechsel <b id='v_rot'>" + String(cfgRotateSec) + " Sek</b></div>";
    html += "<input type='range' name='rotate_sec' min='2' max='30' value='" + String(cfgRotateSec) + "' oninput=\"document.getElementById('v_rot').textContent=this.value+' Sek'\"></div>";

    html += "<div class='setting'><div class='setting-label'>Helligkeit <b id='v_bri'>" + String(cfgBrightness * 100 / 255) + "%</b></div>";
    html += "<input type='range' name='brightness' min='10' max='255' value='" + String(cfgBrightness) + "' oninput=\"document.getElementById('v_bri').textContent=Math.round(this.value*100/255)+'%'\"></div>";

    String nightFromVal = (cfgNightFrom >= 0) ? String(cfgNightFrom) : "22";
    String nightToVal   = (cfgNightTo   >= 0) ? String(cfgNightTo)   : "7";
    String nightBrVal   = String(cfgNightBright * 100 / 255);
    bool nightOn = (cfgNightFrom >= 0);
    html += "<div class='setting'>";
    html += "<label class='check-label'><input type='checkbox' name='night_on' value='1' id='cb_night'";
    if (nightOn) html += " checked";
    html += " onchange=\"document.getElementById('night_opts').style.display=this.checked?'block':'none'\">Nachtmodus</label></div>";

    html += "<div id='night_opts' style='display:";
    html += nightOn ? "block" : "none";
    html += "'>";
    html += "<div class='time-row'>";
    html += "<span>Von</span><input type='number' name='night_from' min='0' max='23' value='" + nightFromVal + "'>";
    html += "<span>bis</span><input type='number' name='night_to' min='0' max='23' value='" + nightToVal + "'><span>Uhr</span></div>";
    html += "<div class='setting-label'>Nacht-Helligkeit <b id='v_nbri'>" + nightBrVal + "%</b></div>";
    html += "<input type='range' name='night_bright' min='0' max='100' value='" + nightBrVal + "' oninput=\"document.getElementById('v_nbri').textContent=this.value+'%'\">";
    html += "</div>";

    html += "<div class='options-col'>";
    html += "<label class='check-label'><input type='checkbox' name='show_next' value='1'";
    if (cfgShowNext) html += " checked";
    html += ">Nächste Abfahrt anzeigen</label>";
    html += "<label class='check-label'><input type='checkbox' name='show_disruptions' value='1'";
    if (cfgShowDisruptions) html += " checked";
    html += ">Störungsticker anzeigen</label>";
    html += "</div>";

    html += "<button class='btn-secondary' type='submit' style='margin-top:14px'>Speichern</button>";
    html += "</form></div>";

    html += "<div class='card'><h2>WiFi</h2>";
    html += "<p class='info-text'>Verbunden mit: <b>" + WiFi.SSID() + "</b></p>";
    html += "<p class='hint'>Nur 2,4 GHz Netzwerke werden unterstützt.</p>";
    html += "<form action='/wifi-reset' method='POST'>";
    html += "<button class='btn-warn'>WLAN ändern</button>";
    html += "</form></div>";

    html += "<div class='card'><h2>Zurücksetzen</h2>";
    html += "<form action='/factory-reset' method='POST' onsubmit=\"return confirm('Wirklich alles löschen? Alle Linien und Einstellungen gehen verloren.')\">";
    html += "<button class='btn-danger'>Werksreset</button>";
    html += "</form></div>";

    html += "<div class='card'><h2>Firmware</h2>";
    html += "<p class='info-text'>Version: <b>v" + String(FW_VERSION) + "</b></p>";
    html += "<a href='/update'><button class='btn-info'>Nach Update suchen</button></a>";
    html += "</div>";

    html += "<div class='footer'>";
    html += "<b>LineTracker</b> by Leo Blum<br>";
    html += "Daten: <a href='https://data.wien.gv.at'>Stadt Wien &ndash; data.wien.gv.at</a> (CC BY 4.0)";
    html += " &middot; <a href='https://fahrplan.oebb.at'>ÖBB/SCOTTY</a>";
    html += "</div>";

    html += "</body></html>";
    sendHtml(html);
}

void handleSearch() {
    String query = server.arg("q");
    if (query.length() < 2) {
        server.sendHeader("Location", "/");
        server.send(302);
        return;
    }

    unsigned long t0 = millis();
    auto stations = searchStations(query);
    logf("[search] q='%s' csv scan: %lums, stations: %d\n", query.c_str(), millis()-t0, stations.size());

    if (stations.empty()) {
        String html = FPSTR(HTML_HEAD);
        html += "<div class='card'><p class='status'>Keine Station gefunden für:<br><b style='color:#e8e8f0'>" + query + "</b></p>";
        html += "<p class='hint' style='text-align:center'>Tipp: Versuche einen kürzeren Suchbegriff oder <a href='/browse' style='color:#ffbf00'>blättere durch alle Stationen</a>.</p>";
        html += "<a href='/'><button>Zurück</button></a></div></body></html>";
        sendHtml(html);
        return;
    }

    // Hybrid search: static CSV data + dirCache + live API probe
    // steige.csv only lists ONE line per platform, but multiple lines share platforms
    // dirCache (from live departures) and API probe discover the real line set

    struct SearchResult {
        String rbl;
        String lineName;
        String towards;
        String type;
    };
    std::vector<SearchResult> results;
    std::map<String, bool> seenLineDir;  // dedupe by lineName|towards

    // Collect all RBLs — read steige.csv once for all matched stations
    unsigned long t1 = millis();
    std::vector<String> haltIds;
    for (auto& st : stations) haltIds.push_back(st.first);
    auto steigeByHalt = findSteigeForStations(haltIds);
    std::vector<SteigeInfo> allSteige;
    for (auto& kv : steigeByHalt)
        for (auto& s : kv.second) {
            bool dup = false;
            for (auto& a : allSteige) { if (a.rbl == s.rbl && a.linienId == s.linienId) { dup = true; break; } }
            if (!dup) allSteige.push_back(s);
        }
    logf("[search] steige scan: %lums, rbls: %d\n", millis()-t1, allSteige.size());

    // Layer 1: Add lines from dirCache (most accurate — from live departures)
    // uncachedRbls: RBLs not in dirCache AND not resolvable by lineDirMap (truly unknown)
    std::vector<String> uncachedRbls;
    for (auto& si : allSteige) {
        auto it = dirCache.find(si.rbl);
        if (it != dirCache.end()) {
            for (auto& fl : it->second) {
                String key = fl.lineName + "|" + si.richtung;
                if (seenLineDir.find(key) == seenLineDir.end()) {
                    seenLineDir[key] = true;
                    results.push_back({si.rbl, fl.lineName, fl.towards, fl.type});
                }
            }
        } else if (lineDirMap.find(si.linienId) == lineDirMap.end()) {
            // Not in dirCache and lineDirMap can't resolve it either → probe live API
            uncachedRbls.push_back(si.rbl);
        }
    }

    // Layer 2: Add lines from static lineDirMap (for lines not yet in dirCache)
    for (auto& si : allSteige) {
        auto it = lineDirMap.find(si.linienId);
        if (it == lineDirMap.end()) continue;
        String towards = (si.richtung == "H") ? it->second.terminusH : it->second.terminusR;
        if (towards.length() == 0) towards = (si.richtung == "H") ? "Richtung H" : "Richtung R";
        String key = it->second.name + "|" + si.richtung;
        if (seenLineDir.find(key) != seenLineDir.end()) continue;
        seenLineDir[key] = true;
        results.push_back({si.rbl, it->second.name, towards, it->second.type});
    }

    logf("[search] L1+L2: %d results, uncached: %d, lineDirMap: %d\n", results.size(), uncachedRbls.size(), lineDirMap.size());

    // Layer 3: Probe live API for RBLs that neither dirCache nor lineDirMap could resolve.
    // This covers: new lines not yet in the CSV, and first-boot before lineDirMap is built.
    if (!uncachedRbls.empty() && WiFi.status() == WL_CONNECTED) {
        unsigned long t3 = millis();
        auto probed = probeRbls(uncachedRbls);
        logf("[search] L3 probe: %lums, found: %d\n", millis()-t3, probed.size());
        for (auto& fl : probed) {
            String key = fl.lineName + "|" + fl.towards;
            if (seenLineDir.find(key) != seenLineDir.end()) continue;
            seenLineDir[key] = true;
            results.push_back({fl.rbl, fl.lineName, fl.towards, fl.type});
        }
    }

    logf("[search] total: %lums, %d results\n", millis()-t0, results.size());

    // Sort by transport type: U-Bahn → Tram → Bus → Train → other
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        int pa = transportPriority(a.type), pb = transportPriority(b.type);
        if (pa != pb) return pa < pb;
        return a.lineName < b.lineName;
    });

    String html = FPSTR(HTML_HEAD);
    html += "<div class='card'><h2>Ergebnisse: " + query + "</h2>";

    if (lineDirMap.empty()) {
        html += "<p class='hint' style='color:#ffbf00'>&#9888; Liniendatenbank wird noch geladen &mdash; Ergebnisse m&ouml;glicherweise unvollst&auml;ndig. Seite in ca. 1 Minute neu laden.</p>";
    }

    if (results.empty()) {
        html += "<p class='status'>Keine Linien gefunden</p>";
    } else {
        html += "<form action='/save' method='POST'>";
        for (size_t i = 0; i < results.size(); i++) {
            auto& sr = results[i];
            bool alreadyActive = false;
            for (auto& cl : cfgLines) {
                if (cl.rbl == sr.rbl && cl.name == sr.lineName) { alreadyActive = true; break; }
            }

            String val = sr.rbl + "|" + sr.lineName + "|" + sr.towards + "|" + sr.type;
            html += "<div class='line-item'>";
            html += "<input type='checkbox' name='line' value='" + val + "'";
            if (alreadyActive) html += " checked disabled";
            html += ">";
            html += "<span class='" + badgeClassForType(sr.type) + "'>" + sr.lineName + "</span>";
            html += "<div class='dir'>" + sr.towards + "</div>";
            html += "</div>";
        }
        html += "<button class='add' type='submit'>Ausgewählte hinzufügen</button>";
        html += "</form>";
    }

    html += "<br><a href='/'><button>Zurück</button></a></div></body></html>";
    sendHtml(html);
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
                if (existing.rbl == cl.rbl && existing.name == cl.name) { dup = true; break; }
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
    
        html += "<div class='card'><p class='status'>ÖBB-Station nicht gefunden: " + query + "</p>";
        html += "<a href='/'><button>Zurück</button></a></div></body></html>";
        sendHtml(html);
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
        html += "<button class='add' type='submit'>Ausgewählte hinzufügen</button>";
        html += "</form>";
    }

    html += "<br><a href='/'><button>Zurück</button></a></div></body></html>";
    sendHtml(html);
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

    if (letter.length() >= 1) {
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

                // Map Umlauts to base letter for browse matching
                String normalized = normalizeForSearch(name.substring(0, 2));  // first char (UTF-8 may be 2 bytes)
                String firstNorm = normalized.substring(0, 1);
                firstNorm.toUpperCase();
                if (firstNorm != letter) continue;

                if (seen.find(name) != seen.end()) continue;
                seen[name] = true;
                names.push_back(name);
            }
            f.close();

            std::sort(names.begin(), names.end());

            if (names.empty()) {
                html += "<p class='status'>Keine Stationen mit " + letter + "</p>";
            } else {
                html += "<p class='stn-count'>" + String(names.size()) + " Stationen</p>";
                for (auto& n : names) {
                    String encoded = n;
                    encoded.replace(" ", "+");
                    html += "<a class='stn' href='/search?q=" + encoded + "'>" + n + "</a>";
                }
            }
        } else {
            html += "<p class='status'>Stationsdaten nicht verfügbar. Bitte neu starten.</p>";
        }
    } else {
        html += "<p class='status'>Wähle einen Buchstaben</p>";
    }

    html += "<br><a href='/'><button>Zurück</button></a></div></body></html>";
    sendHtml(html);
}

void handleWifiReset() {
    // Set flag so setup() opens config portal on restart
    File f = SPIFFS.open(WIFI_RESET_FLAG, "w");
    if (f) { f.print("1"); f.close(); }

    String html = FPSTR(HTML_HEAD);
    html += "<div class='card'><h2>WiFi Reset</h2><p class='status'>Monitor startet neu...<br><br>";
    html += "Verbinde dich mit:<br><b style='color:#ffbf00;font-size:1.3em'>LineTracker</b><br><br>";
    html += "Wähle dein WLAN, dann öffne:<br><b style='color:#ffbf00'>linetracker.local</b></p></div></body></html>";
    sendHtml(html);
    delay(1500);
    ESP.restart();
}

void handleFactoryReset() {
    SPIFFS.remove(CONFIG_PATH);
    SPIFFS.remove(DIR_CACHE_PATH);
    SPIFFS.remove(LINE_DIRS_PATH);
    SPIFFS.remove(CACHE_HALT_PATH);
    SPIFFS.remove(CACHE_STEIGE_PATH);
    SPIFFS.remove(CACHE_LINIEN_PATH);
    SPIFFS.remove(CACHE_TS_PATH);

    WiFiManager wm;
    wm.resetSettings();

    String html = FPSTR(HTML_HEAD);
    html += "<div class='card'><h2>Werksreset</h2><p class='status'>Alles gelöscht!<br><br>";
    html += "Monitor startet neu...<br><br>";
    html += "Verbinde dich mit:<br><b style='color:#ffbf00;font-size:1.3em'>LineTracker</b><br><br>";
    html += "Wähle dein WLAN, dann öffne:<br><b style='color:#ffbf00'>linetracker.local</b></p></div></body></html>";
    sendHtml(html);
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

    // Signal display task to show OTA progress
    otaNewVersion = remoteVer;
    otaPercent = 0;
    otaInProgress = true;

    WiFiClientSecure updClient;
    updClient.setInsecure();

    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    httpUpdate.onProgress([](int cur, int total) {
        if (total > 0) otaPercent = cur * 100 / total;
    });
    t_httpUpdate_return ret = httpUpdate.update(updClient, binUrl);

    otaInProgress = false;
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

// Check version only (no flash), returns remote version or empty
String checkRemoteVersion() {
    if (WiFi.status() != WL_CONNECTED) return "";
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, OTA_VERSION_URL);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("OTA check: HTTP %d\n", code);
        http.end();
        return "";
    }
    String payload = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return "";
    return doc["version"] | "";
}

void handleOtaCheck() {
    String remoteVer = checkRemoteVersion();
    String html = FPSTR(HTML_HEAD);
    html += "<div class='card'><h2>Firmware Update</h2>";
    html += "<p style='font-size:13px;color:#aaa;margin-bottom:12px'>Installiert: <b style='color:#eee'>v" + String(FW_VERSION) + "</b></p>";

    if (remoteVer.length() == 0) {
        html += "<p class='status'>Updateserver nicht erreichbar.</p>";
    } else if (isNewerVersion(remoteVer, FW_VERSION)) {
        html += "<p style='text-align:center;color:#1a8f3c;font-size:16px;font-weight:600;margin:12px 0'>v" + remoteVer + " verfügbar!</p>";
        html += "<a href='/update-now'><button class='add'>Jetzt updaten</button></a>";
    } else {
        html += "<p class='status'>Firmware ist aktuell.</p>";
    }

    html += "<br><a href='/'><button class='btn-secondary'>Zurück</button></a>";
    html += "</div></body></html>";
    sendHtml(html);
}

void handleOtaDoUpdate() {
    String html = FPSTR(HTML_HEAD);
    html += "<div class='card' style='text-align:center'>";
    html += "<h2>Update wird installiert...</h2>";
    html += "<p style='color:#aaa;font-size:14px;margin:16px 0'>Bitte nicht ausschalten!<br>Der Monitor startet automatisch neu.</p>";
    html += "<div style='margin:20px auto;width:80%;height:10px;background:#222;border-radius:5px'>";
    html += "<div id='bar' style='width:0%;height:100%;background:#ffbf00;border-radius:5px;transition:width .5s'></div></div>";
    html += "<p id='pct' style='color:#ffbf00;font-size:20px;font-weight:700'>0%</p>";
    html += "</div>";
    html += "<script>"
            "var iv=setInterval(function(){"
            "fetch('/update-progress').then(r=>r.json()).then(d=>{"
            "document.getElementById('bar').style.width=d.p+'%';"
            "document.getElementById('pct').textContent=d.p+'%';"
            "if(d.p>=100){clearInterval(iv);document.getElementById('pct').textContent='Neustart...';}"
            "}).catch(function(){clearInterval(iv);"
            "document.getElementById('pct').textContent='Neustart...';"
            "setTimeout(function(){location.href='/';},8000);"
            "})},800);"
            "</script></body></html>";
    sendHtml(html);
    delay(200);
    checkOtaUpdate();
}

void handleOtaProgress() {
    String json = "{\"p\":" + String(otaPercent) + "}";
    server.send(200, "application/json", json);
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
    server.on("/factory-reset", HTTP_POST, handleFactoryReset);
    server.on("/update", handleOtaCheck);
    server.on("/update-now", handleOtaDoUpdate);
    server.on("/update-progress", handleOtaProgress);
    // Captive portal detection — iOS/Android send these to check for portal
    auto captiveRedirect = []() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    };
    server.on("/hotspot-detect.html", captiveRedirect);   // iOS
    server.on("/generate_204", captiveRedirect);           // Android
    server.on("/connecttest.txt", captiveRedirect);        // Windows
    server.onNotFound([captiveRedirect]() {
        if (portalOpen) captiveRedirect();
        else server.send(404, "text/plain", "Not found");
    });
    server.on("/logs", []() {
        String html = "<html><head><meta charset='utf-8'>"
                      "<meta http-equiv='refresh' content='2'>"
                      "<style>body{background:#0a0805;color:#ffbf00;font-family:monospace;"
                      "font-size:13px;padding:12px;white-space:pre-wrap;word-break:break-all}"
                      "a{color:#ffbf00}</style></head><body>"
                      "<a href='/'>← Home</a>  <a href='/logs'>⟳ Refresh</a>\n\n";
        html += getLogContents();
        html += "</body></html>";
        server.send(200, "text/html; charset=utf-8", html);
    });
    server.begin();
    configMode = true;
}

// ── WiFiManager ──────────────────────────────────────────────────────
void showWifiSetupScreen(const char* subtitle) {
    tft.fillScreen(BG_COLOR);
    tft.setTextFont(1);

    // "LineTracker" branding
    tft.setTextColor(AMBER, BG_COLOR);
    tft.setTextSize(3);
    const char* brand = "LineTracker";
    tft.setCursor((320 - tft.textWidth(brand)) / 2, 8);
    tft.print(brand);

    tft.drawFastHLine(60, 35, 200, AMBER_DIM);

    // Subtitle
    tft.setTextColor(AMBER_DIM, BG_COLOR);
    tft.setTextSize(1);
    int sw = tft.textWidth(subtitle);
    tft.setCursor((320 - sw) / 2, 42);
    tft.print(subtitle);

    // Instructions
    tft.setTextColor(AMBER, BG_COLOR);
    tft.setTextSize(2);
    const char* s1 = "1) WLAN verbinden:";
    tft.setCursor((320 - tft.textWidth(s1)) / 2, 56);
    tft.print(s1);

    tft.setTextColor(tft.color565(255, 220, 60), BG_COLOR);
    const char* ap = "\"LineTracker\"";
    tft.setCursor((320 - tft.textWidth(ap)) / 2, 76);
    tft.print(ap);

    tft.setTextColor(AMBER, BG_COLOR);
    const char* s2 = "2) Netzwerk waehlen";
    tft.setCursor((320 - tft.textWidth(s2)) / 2, 100);
    tft.print(s2);

    tft.setTextColor(AMBER_DIM, BG_COLOR);
    tft.setTextSize(1);
    const char* hint = "Nur 2.4 GHz | Timeout: 3 Min";
    tft.setCursor((320 - tft.textWidth(hint)) / 2, 128);
    tft.print(hint);

    // Attribution
    tft.setTextColor(tft.color565(30, 24, 5), BG_COLOR);
    const char* attr = "by Leo Blum";
    tft.setCursor((320 - tft.textWidth(attr)) / 2, 158);
    tft.print(attr);
}

void setupWiFi() {
    // Force 2.4 GHz and maximize compatibility
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    wm.setConnectRetries(3);              // retry connection up to 3 times
    wm.setMinimumSignalQuality(10);       // accept weak signals

    bool wifiResetRequested = SPIFFS.exists(WIFI_RESET_FLAG);
    if (wifiResetRequested) {
        SPIFFS.remove(WIFI_RESET_FLAG);
    }

    if (wifiResetRequested) {
        showWifiSetupScreen("WLAN ändern");
        wm.setConfigPortalTimeout(180);
        if (!wm.startConfigPortal("LineTracker")) {
            tft.fillRect(0, 135, 320, 25, BG_COLOR);
            tft.setTextColor(AMBER_DIM, BG_COLOR);
            tft.setTextFont(1);
            tft.setTextSize(1);
            const char* msg = "Verbinde mit altem WLAN...";
            tft.setCursor((320 - tft.textWidth(msg)) / 2, 140);
            tft.print(msg);
            WiFi.begin();
            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
                delay(500);
            }
            if (WiFi.status() != WL_CONNECTED) {
                ESP.restart();
            }
        }
    } else {
        wm.setAPCallback([](WiFiManager* mgr) {
            showWifiSetupScreen("Ersteinrichtung");
        });
        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        wm.setConnectTimeout(15);         // 15s for saved WiFi (was 5s)
        wm.setSaveConnectTimeout(30);     // wait 30s when user submits credentials in portal
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
    std::map<String, bool> seen;
    for (auto& cl : cfgLines) {
        if (!seen[cl.rbl]) { url += "&rbl=" + cl.rbl; seen[cl.rbl] = true; }
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
    filter["data"]["monitors"][0]["locationStop"]["properties"]["title"] = true;
    filter["data"]["monitors"][0]["locationStop"]["properties"]["attributes"]["rbl"] = true;
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

    // Build set of configured (rbl|lineName) pairs for filtering
    std::map<String, bool> cfgLineSet;
    for (auto& cl : cfgLines) cfgLineSet[cl.rbl + "|" + cl.name] = true;

    std::vector<Departure> newDeps;
    bool dirCacheUpdated = false;
    JsonArray monitors = doc["data"]["monitors"].as<JsonArray>();
    for (JsonObject monitor : monitors) {
        String stopName = monitor["locationStop"]["properties"]["title"] | "";
        String rbl = String((int)(monitor["locationStop"]["properties"]["attributes"]["rbl"] | 0));
        JsonArray lines = monitor["lines"].as<JsonArray>();
        for (JsonObject line : lines) {
            String name    = line["name"].as<String>();
            String towards = line["towards"].as<String>();
            String type    = line["type"].as<String>();
            bool   rt      = line["realtimeSupported"] | false;

            // Cache direction info from live data
            if (rbl.length() > 0 && name.length() > 0 && towards.length() > 0) {
                size_t szBefore = dirCache[rbl].size();
                cacheDirEntry(rbl, name, towards, type, stopName);
                if (dirCache[rbl].size() != szBefore) dirCacheUpdated = true;
            }

            // Only include departures for configured lines
            if (!cfgLineSet[rbl + "|" + name]) continue;

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

    if (dirCacheUpdated) saveDirCache();

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
        logf("OeBB: NTP not available\n");
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

    // ── OTA update progress screen ──
    if (otaInProgress) {
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(3);
        const char* brand = "LineTracker";
        int tw = sprite.textWidth(brand);
        sprite.setCursor((SCREEN_W - tw) / 2, 6);
        sprite.print(brand);

        sprite.drawFastHLine(40, 38, 240, AMBER_DIM);

        sprite.setTextSize(2);
        const char* msg = "Firmware Update";
        tw = sprite.textWidth(msg);
        sprite.setCursor((SCREEN_W - tw) / 2, 48);
        sprite.print(msg);

        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        sprite.setTextSize(1);
        String verStr = "v" + String(FW_VERSION) + " -> v" + otaNewVersion;
        tw = sprite.textWidth(verStr);
        sprite.setCursor((SCREEN_W - tw) / 2, 72);
        sprite.print(verStr);

        // Progress bar
        int barX = 30, barY = 92, barW = 260, barH = 16;
        sprite.drawRect(barX, barY, barW, barH, AMBER_DIM);
        int fillW = (barW - 4) * otaPercent / 100;
        if (fillW > 0) sprite.fillRect(barX + 2, barY + 2, fillW, barH - 4, AMBER);

        // Percentage
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(2);
        String pctStr = String(otaPercent) + "%";
        tw = sprite.textWidth(pctStr);
        sprite.setCursor((SCREEN_W - tw) / 2, 118);
        sprite.print(pctStr);

        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        sprite.setTextSize(1);
        const char* warn = "Nicht ausschalten!";
        tw = sprite.textWidth(warn);
        sprite.setCursor((SCREEN_W - tw) / 2, 150);
        sprite.print(warn);

        sprite.pushSprite(0, 0);
        sprite.deleteSprite();
        return;
    }

    // ── WiFi down screen ──
    if (WiFi.status() != WL_CONNECTED) {
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(3);
        const char* title = "Kein WLAN";
        int tw = sprite.textWidth(title);
        sprite.setCursor((SCREEN_W - tw) / 2, 18);
        sprite.print(title);

        sprite.drawFastHLine(40, 52, 240, AMBER_DIM);

        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        sprite.setTextSize(1);
        const char* sub1 = "Verbindet automatisch";
        tw = sprite.textWidth(sub1);
        sprite.setCursor((SCREEN_W - tw) / 2, 68);
        sprite.print(sub1);
        const char* sub2 = "wenn WLAN wieder verfuegbar ist.";
        tw = sprite.textWidth(sub2);
        sprite.setCursor((SCREEN_W - tw) / 2, 84);
        sprite.print(sub2);

        sprite.drawFastHLine(40, 110, 240, AMBER_DIM);

        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        const char* h1 = "Mit \"LineTracker\" verbinden,";
        tw = sprite.textWidth(h1);
        sprite.setCursor((SCREEN_W - tw) / 2, 118);
        sprite.print(h1);
        const char* h2 = "dann linetracker.local oeffnen.";
        tw = sprite.textWidth(h2);
        sprite.setCursor((SCREEN_W - tw) / 2, 134);
        sprite.print(h2);

        sprite.pushSprite(0, 0);
        sprite.deleteSprite();
        return;
    }

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    int totalSlots = displaySlots.size();

    if (cfgLines.empty() && cfgOebb.empty()) {
        // ── Setup screen (320×170) ──
        // LineTracker branding
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(4);
        const char* brand = "LineTracker";
        int tw = sprite.textWidth(brand);
        sprite.setCursor((SCREEN_W - tw) / 2, 6);
        sprite.print(brand);

        sprite.drawFastHLine(40, 42, 240, AMBER_DIM);

        // URL prominent
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(2);
        String hostnameStr = cfgHostname + ".local";
        tw = sprite.textWidth(hostnameStr);
        sprite.setCursor((SCREEN_W - tw) / 2, 54);
        sprite.print(hostnameStr);

        // IP fallback
        sprite.setTextColor(AMBER_DIM, BG_COLOR);
        sprite.setTextSize(1);
        String ip = WiFi.localIP().toString();
        tw = sprite.textWidth(ip);
        sprite.setCursor((SCREEN_W - tw) / 2, 80);
        sprite.print(ip);

        // Instruction
        sprite.drawFastHLine(80, 98, 160, AMBER_DIM);
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(2);
        const char* s3 = "Linien auswaehlen";
        tw = sprite.textWidth(s3);
        sprite.setCursor((SCREEN_W - tw) / 2, 108);
        sprite.print(s3);

        // "by Leo Blum" subtle
        sprite.setTextColor(tft.color565(30, 24, 5), BG_COLOR);
        sprite.setTextSize(1);
        const char* attr = "by Leo Blum";
        tw = sprite.textWidth(attr);
        sprite.setCursor((SCREEN_W - tw) / 2, 158);
        sprite.print(attr);
    } else if (totalSlots == 0) {
        // ── Loading screen ──
        sprite.setTextColor(AMBER, BG_COLOR);
        sprite.setTextSize(2);
        const char* msg = fetchError ? "Daten werden geladen..." : "Lade Abfahrten...";
        int tw = sprite.textWidth(msg);
        sprite.setCursor((SCREEN_W - tw) / 2, (SCREEN_H - 14) / 2);
        sprite.print(msg);
        // Show IP as fallback for web access
        String ip = WiFi.localIP().toString();
        if (ip != "0.0.0.0") {
            sprite.setTextColor(AMBER_DIM, BG_COLOR);
            sprite.setTextSize(1);
            int iw = sprite.textWidth(ip);
            sprite.setCursor((SCREEN_W - iw) / 2, (SCREEN_H - 14) / 2 + 22);
            sprite.print(ip);
        }
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

        // ── IP address (tiny, top-right, always visible as fallback) ──
        {
            String ip = WiFi.localIP().toString();
            sprite.setTextFont(1);
            sprite.setTextSize(1);
            sprite.setTextColor(tft.color565(25, 20, 5), BG_COLOR);
            int iw = sprite.textWidth(ip);
            sprite.setCursor(SCREEN_W - iw - 2, 2);
            sprite.print(ip);
        }

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
    // Wait for NTP sync (non-blocking, max 4s)
    {
        struct tm ti;
        int ntpWaits = 0;
        while (!getLocalTime(&ti, 1000) && ntpWaits < 4) ntpWaits++;
        logf("%s\n", getLocalTime(&ti, 0) ? "NTP synced" : "NTP timeout, will retry");
    }

    unsigned long lastOtaCheck = millis();
    for (;;) {
        // WiFi state tracking — ESP32 auto-reconnects, we just manage the portal
        if (WiFi.status() != WL_CONNECTED) {
            if (wifiDownSince == 0) {
                wifiDownSince = millis();
                logf("WiFi lost, requesting portal\n");
                portalShouldOpen = true;
            }
        } else if (wifiDownSince != 0) {
            logf("WiFi reconnected: %s\n", WiFi.localIP().toString().c_str());
            wifiDownSince = 0;
            if (portalOpen) {
                portalOpen = false;
                portalShouldOpen = false;
                logf("Closing AP after reconnect\n");
            }
            MDNS.begin(cfgHostname.c_str());
            MDNS.addService("http", "tcp", 80);
        }
        fetchDepartures();
        fetchOebbDepartures();
        // Refresh CSV cache if stale, then rebuild line directions
        if (!isCacheValid()) {
            if (refreshCsvCache(true)) buildLineDirections();
        } else if ((lineDirMap.empty() || haltRecordCount == 0) && SPIFFS.exists(CACHE_STEIGE_PATH)) {
            // Cache valid but line directions or search indexes not built yet
            buildLineDirections();
        }
        // Check for OTA updates every 6h
        if (millis() - lastOtaCheck > OTA_CHECK_INTERVAL_MS) {
            checkOtaUpdate();
            lastOtaCheck = millis();
        }
        // Wait 20s, but wake early if config changed or WiFi drops
        for (int t = 0; t < 40; t++) {  // 40 × 500ms = 20s
            if (configChanged) {
                configChanged = false;
                break;
            }
            if (WiFi.status() != WL_CONNECTED && wifiDownSince == 0) {
                wifiDownSince = millis();
                logf("WiFi lost (sleep), requesting portal\n");
                portalShouldOpen = true;
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
    logf("LineTracker v%s starting...\n", FW_VERSION);

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

    if (!SPIFFS.begin(true)) logf("SPIFFS mount failed\n");

    loadConfig();
    if (cfgHostname.length() == 0) {
        cfgHostname = "linetracker";
        saveConfig();
    }
    loadDirCache();
    loadLineDirections();
    ledcWrite(0, cfgBrightness);

    // Show loading status below splash
    tft.setTextColor(AMBER_DIM, BG_COLOR);
    tft.setTextSize(1);
    tft.setCursor((320 - tft.textWidth("Verbinde WiFi...")) / 2, 140);
    tft.print("Verbinde WiFi...");

    setupWiFi();

    // Give the WiFi stack a moment to fully stabilize before mDNS check
    delay(500);

    // Detect hostname conflict: check if linetracker.local is already in use by
    // another device. If so, assign a unique MAC-based name. If no conflict and
    // we previously had a MAC-based name, revert to "linetracker".
    {
        IPAddress resolved;
        bool conflict = WiFi.hostByName("linetracker.local", resolved)
                        && resolved != WiFi.localIP()
                        && resolved != IPAddress(0, 0, 0, 0);
        if (conflict) {
            uint8_t mac[6];
            WiFi.macAddress(mac);
            char suffix[5];
            snprintf(suffix, sizeof(suffix), "%02x%02x", mac[4], mac[5]);
            String uniqueName = "linetracker-" + String(suffix);
            if (cfgHostname != uniqueName) {
                cfgHostname = uniqueName;
                saveConfig();
                logf("Hostname conflict — using: %s\n", cfgHostname.c_str());
            }
        } else if (cfgHostname != "linetracker") {
            // No conflict — revert to default if we had a MAC-based name
            cfgHostname = "linetracker";
            saveConfig();
        }
    }

    // Start mDNS + web server immediately so user can access UI
    if (MDNS.begin(cfgHostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        logf("mDNS: %s.local\n", cfgHostname.c_str());
    }
    logf("Connected! IP: %s heap=%u psram=%u\n",
         WiFi.localIP().toString().c_str(), ESP.getFreeHeap(), ESP.getFreePsram());
    startConfigServer();

    // Briefly show assigned hostname on splash so user always knows the URL
    {
        String urlStr = "http://" + cfgHostname + ".local";
        tft.fillRect(0, 130, 320, 40, BG_COLOR);
        tft.setTextFont(1);
        tft.setTextColor(AMBER, BG_COLOR);
        tft.setTextSize(2);
        int uw = tft.textWidth(urlStr);
        tft.setCursor((320 - uw) / 2, 138);
        tft.print(urlStr);
        delay(3000);
    }

    // Start display task immediately so setup screen with IP shows
    dataMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(displayTask, "display", 8192,  NULL, 1, NULL, 1);

    // NTP config (sync happens in background, no blocking wait)
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

    logf("WL Lines: %d\n", cfgLines.size());
    logf("OeBB Stations: %d\n", cfgOebb.size());

    // Start data fetch task immediately (handles NTP wait, CSV cache, line directions)
    xTaskCreatePinnedToCore(dataTask,    "data",    16384, NULL, 1, NULL, 0);
}

void loop() {
    server.handleClient();
    if (portalShouldOpen && !portalOpen) {
        portalShouldOpen = false;
        // Open raw soft AP without touching STA reconnect.
        // DNS server redirects all domains → 192.168.4.1 so phones auto-open captive portal.
        // WiFiManager is NOT involved here — it would call WiFi.disconnect() and break reconnect.
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("LineTracker");
        WiFi.begin(); // restart STA with saved NVS credentials
        apDns.start(53, "*", WiFi.softAPIP());
        portalOpen = true;
        logf("AP opened at %s, STA reconnecting\n", WiFi.softAPIP().toString().c_str());
    }
    if (portalOpen) {
        apDns.processNextRequest();
        if (WiFi.status() == WL_CONNECTED) {
            apDns.stop();
            WiFi.softAPdisconnect(true);
            portalOpen = false;
            logf("AP closed after reconnect\n");
        }
    }
    checkResetButton();
    delay(5);
}
