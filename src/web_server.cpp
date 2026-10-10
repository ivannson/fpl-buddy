#include "web_server.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <new>

#include "device_config.h"
#include "display_settings.h"
#include "fpl_config.h"
#include "web_ui_html.h"

static constexpr const char *kAuthUser = "admin";
static constexpr size_t kMaxLeagues = 32;

// Handler buffers live in PSRAM: internal RAM is kept free for TLS (see changelog "TLS out of internal RAM").
// Only the server task touches them.
struct Scratch {
    DeviceConfig config;
    DisplaySettings settings;
    DisplaySettings saved;
    WebLeagueInfo leagues[kMaxLeagues];
};
static Scratch *scratch = nullptr;
static TaskHandle_t serverTaskHandle = nullptr;

static WebServer server(80);
static DNSServer dnsServer;
static bool started = false;
static bool setupMode = false;
static volatile bool otaInProgress = false;
static uint32_t restartAtMs = 0;

// Upload state (only touched from the server task).
static bool uploadStarted = false;
static bool uploadAuthorized = false;
static String uploadError;

static void scheduleRestart() {
    restartAtMs = millis() + 1000;
}

static void sendJson(int code, const JsonDocument &doc) {
    String body;
    serializeJson(doc, body);
    server.send(code, "application/json", body);
}

static void sendError(int code, const char *message) {
    StaticJsonDocument<192> doc;
    doc["error"] = message;
    sendJson(code, doc);
}

static bool isAuthorized() {
    DeviceConfig &config = scratch->config;
    deviceConfigGet(config);
    return !config.webPassword[0] || server.authenticate(kAuthUser, config.webPassword);
}

static bool requireAuth() {
    if (isAuthorized()) {
        return true;
    }
    server.requestAuthentication(BASIC_AUTH, "FPL Buddy");
    return false;
}

// Parses the request body; sends a 400 and returns false on bad JSON.
static bool parseBody(JsonDocument &doc) {
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        sendError(400, "invalid JSON");
        return false;
    }
    return true;
}

static void handleRoot() {
    if (!requireAuth()) {
        return;
    }
    server.send_P(200, "text/html", kWebUiHtml);
}

static void handleState() {
    if (!requireAuth()) {
        return;
    }
    DeviceConfig &config = scratch->config;
    DisplaySettings &settings = scratch->settings;
    WebLeagueInfo *leagues = scratch->leagues;
    deviceConfigGet(config);
    displaySettingsGet(settings);
    size_t leagueCount = 0;
    const bool hasLeagues = appGetMiniLeagues(leagues, kMaxLeagues, leagueCount);

    DynamicJsonDocument doc(6144);
    doc["firmware"] = FPL_FIRMWARE_VERSION;
    doc["mode"] = setupMode ? "setup" : "station";
    doc["hostname"] = kWebHostname;
    doc["ssid"] = config.wifiSsid;
    doc["connected"] = WiFi.status() == WL_CONNECTED;
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["uptimeSec"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["entryId"] = config.entryId;
    doc["webPasswordSet"] = config.webPassword[0] != '\0';
    doc["ledRing"] = FPL_LED_RING_ENABLED != 0;

    JsonObject s = doc.createNestedObject("settings");
    s["interval"] = settings.screenIntervalSec;
    s["graph"] = settings.rankGraphGameweeks;
    JsonArray watch = s.createNestedArray("watchlist");
    for (size_t i = 0; i < settings.watchCount; ++i) {
        watch.add(settings.watchlist[i]);
    }
    JsonArray hidden = s.createNestedArray("hiddenLeagues");
    for (size_t i = 0; i < settings.hiddenLeagueCount; ++i) {
        hidden.add(settings.hiddenLeagueIds[i]);
    }
    s["ledRankBright"] = settings.ledRankBrightnessPct;
    s["ledDeadlineBright"] = settings.ledDeadlineBrightnessPct;
    s["ledNotifyBright"] = settings.ledNotificationBrightnessPct;
    s["ledRankLiveGw"] = settings.ledRankOnlyLiveGw;
    s["ledRankMatch"] = settings.ledRankOnlyMatchLive;

    if (hasLeagues) {
        JsonArray arr = doc.createNestedArray("leagues");
        for (size_t i = 0; i < leagueCount; ++i) {
            JsonObject lg = arr.createNestedObject();
            lg["id"] = leagues[i].id;
            lg["name"] = leagues[i].name;
        }
    } else {
        doc["leagues"] = nullptr;
    }
    sendJson(200, doc);
}

static void handleSettings() {
    if (!requireAuth()) {
        return;
    }
    DynamicJsonDocument body(2048);
    if (!parseBody(body)) {
        return;
    }
    DisplaySettings &next = scratch->settings;
    displaySettingsGet(next);
    const uint32_t watchlistVersion = next.watchlistVersion;

    next.screenIntervalSec = body["interval"] | next.screenIntervalSec;
    next.rankGraphGameweeks = body["graph"] | next.rankGraphGameweeks;
    next.ledRankBrightnessPct = body["ledRankBright"] | next.ledRankBrightnessPct;
    next.ledDeadlineBrightnessPct = body["ledDeadlineBright"] | next.ledDeadlineBrightnessPct;
    next.ledNotificationBrightnessPct = body["ledNotifyBright"] | next.ledNotificationBrightnessPct;
    next.ledRankOnlyLiveGw = body["ledRankLiveGw"] | next.ledRankOnlyLiveGw;
    next.ledRankOnlyMatchLive = body["ledRankMatch"] | next.ledRankOnlyMatchLive;
    if (body["watchlist"].is<JsonArray>()) {
        JsonArray watch = body["watchlist"].as<JsonArray>();
        if (watch.size() > kMaxWatchlist) {
            sendError(400, "watchlist holds 8 players max");
            return;
        }
        next.watchCount = 0;
        for (JsonVariant v : watch) {
            const char *entry = v | "";
            if (strlen(entry) >= kWatchEntryLen) {
                sendError(400, "player name too long");
                return;
            }
            strlcpy(next.watchlist[next.watchCount++], entry, kWatchEntryLen);
        }
    }
    if (body["hiddenLeagues"].is<JsonArray>()) {
        JsonArray hidden = body["hiddenLeagues"].as<JsonArray>();
        if (hidden.size() > kMaxHiddenLeagues) {
            sendError(400, "too many hidden leagues");
            return;
        }
        next.hiddenLeagueCount = 0;
        for (JsonVariant v : hidden) {
            next.hiddenLeagueIds[next.hiddenLeagueCount++] = v | 0;
        }
    }

    if (!displaySettingsApply(next)) {
        sendError(400, "invalid settings (interval 5-600 s, graph 0/3/5/10, LED brightness 0-100%)");
        return;
    }
    DisplaySettings &saved = scratch->saved;
    displaySettingsGet(saved);
    if (saved.watchlistVersion != watchlistVersion) {
        appRequestFplPoll();
    }
    handleState();
}

static void handleDevice() {
    if (!requireAuth()) {
        return;
    }
    DynamicJsonDocument body(512);
    if (!parseBody(body)) {
        return;
    }
    DeviceConfig &current = scratch->config;
    deviceConfigGet(current);
    bool restart = false;

    if (body.containsKey("ssid")) {
        const char *ssid = body["ssid"] | "";
        // An omitted password keeps the saved one, so the UI never has to show it.
        const char *password = body.containsKey("wifiPassword") ? (body["wifiPassword"] | "") : current.wifiPassword;
        const bool changed = strcmp(ssid, current.wifiSsid) != 0 || strcmp(password, current.wifiPassword) != 0;
        if (changed && !deviceConfigSetWifi(ssid, password)) {
            sendError(400, "invalid WiFi name or password");
            return;
        }
        restart = restart || changed;
    }
    if (body.containsKey("entryId")) {
        const int entryId = body["entryId"] | 0;
        if (entryId != current.entryId) {
            if (!deviceConfigSetEntryId(entryId)) {
                sendError(400, "invalid FPL team ID");
                return;
            }
            restart = true;  // the FPL cache belongs to the old entry
        }
    }
    if (body.containsKey("webPassword")) {
        if (!deviceConfigSetWebPassword(body["webPassword"] | "")) {
            sendError(400, "web password is 32 characters max");
            return;
        }
    }

    StaticJsonDocument<64> doc;
    doc["restart"] = restart;
    sendJson(200, doc);
    if (restart) {
        deviceConfigRequestSetupMode(false);
        scheduleRestart();
    }
}

static void handleScan() {
    if (!requireAuth()) {
        return;
    }
    const int count = WiFi.scanNetworks();
    DynamicJsonDocument doc(6144);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < count && i < 30; ++i) {
        if (WiFi.SSID(i).isEmpty()) {
            continue;
        }
        JsonObject net = arr.createNestedObject();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    sendJson(200, doc);
}

static void handleRestart() {
    if (!requireAuth()) {
        return;
    }
    const bool setup = server.hasArg("setup");
    if (setup) {
        deviceConfigRequestSetupMode(true);
    }
    server.send(200, "application/json", "{\"restart\":true}");
    scheduleRestart();
}

static void handleDemoGet() {
    if (!requireAuth()) {
        return;
    }
    String body;
    appDemoStateJson(body);
    server.send(200, "application/json", body);
}

static void handleDemoPost() {
    if (!requireAuth()) {
        return;
    }
    if (setupMode) {
        sendError(409, "demo mode is not available in setup mode");
        return;
    }
    DynamicJsonDocument body(512);
    if (!parseBody(body)) {
        return;
    }
    const char *error = nullptr;
    if (!appDemoCommand(body.as<JsonObjectConst>(), error)) {
        sendError(400, error ? error : "demo command failed");
        return;
    }
    handleDemoGet();
}

static void handleUpdateUpload() {
    HTTPUpload &upload = server.upload();
    switch (upload.status) {
        case UPLOAD_FILE_START:
            uploadStarted = true;
            uploadAuthorized = isAuthorized();
            uploadError = "";
            if (!uploadAuthorized) {
                return;
            }
            Serial.printf("[OTA] upload started: %s\n", upload.filename.c_str());
            otaInProgress = true;
            appShowStatus("Updating firmware...", 0x00E5FF);
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                uploadError = Update.errorString();
            }
            break;
        case UPLOAD_FILE_WRITE:
            if (uploadAuthorized && uploadError.isEmpty() &&
                Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                uploadError = Update.errorString();
            }
            break;
        case UPLOAD_FILE_END:
            if (uploadAuthorized && uploadError.isEmpty() && !Update.end(true)) {
                uploadError = Update.errorString();
            }
            Serial.printf("[OTA] upload finished: %u bytes%s%s\n", static_cast<unsigned>(upload.totalSize),
                          uploadError.isEmpty() ? "" : ", error: ", uploadError.c_str());
            break;
        case UPLOAD_FILE_ABORTED:
            // The done handler is not called for an aborted upload, so clean up here.
            if (Update.isRunning()) {
                Update.abort();
            }
            uploadStarted = false;
            if (otaInProgress) {
                otaInProgress = false;
                appShowStatus("Firmware update aborted", 0xFF5A5A);
            }
            Serial.println("[OTA] upload aborted");
            break;
    }
}

static void handleUpdateDone() {
    const bool hadUpload = uploadStarted;
    uploadStarted = false;
    if (!hadUpload) {
        if (requireAuth()) {
            sendError(400, "no firmware file in the request");
        }
        return;
    }
    if (!uploadAuthorized) {
        server.requestAuthentication(BASIC_AUTH, "FPL Buddy");
        return;
    }
    if (!uploadError.isEmpty()) {
        if (Update.isRunning()) {
            Update.abort();
        }
        otaInProgress = false;
        appShowStatus("Firmware update failed", 0xFF5A5A);
        sendError(400, uploadError.c_str());
        return;
    }
    appShowStatus("Update done, restarting", 0x38D39F);
    server.send(200, "application/json", "{\"restart\":true}");
    scheduleRestart();
}

static void handleNotFound() {
    if (setupMode) {
        // Captive portal: phones probe URLs like /generate_204 and open whatever the redirect points at.
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
        return;
    }
    server.send(404, "text/plain", "Not found");
}

static void serverTask(void *) {
    for (;;) {
        if (setupMode) {
            dnsServer.processNextRequest();
        }
        server.handleClient();
        if (restartAtMs && millis() > restartAtMs) {
            Serial.println("[WEB] restarting");
            ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void webServerStart(bool setup) {
    if (started) {
        return;
    }
    void *mem = heap_caps_calloc(1, sizeof(Scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
        Serial.println("[WEB] PSRAM allocation failed, web UI disabled");
        return;
    }
    scratch = new (mem) Scratch();
    started = true;
    setupMode = setup;

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/settings", HTTP_POST, handleSettings);
    server.on("/api/device", HTTP_POST, handleDevice);
    server.on("/api/scan", HTTP_GET, handleScan);
    server.on("/api/restart", HTTP_POST, handleRestart);
    server.on("/api/demo", HTTP_GET, handleDemoGet);
    server.on("/api/demo", HTTP_POST, handleDemoPost);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    server.onNotFound(handleNotFound);
    server.begin();

    if (setupMode) {
        dnsServer.start(53, "*", WiFi.softAPIP());
        Serial.printf("[WEB] setup UI at http://%s/\n", WiFi.softAPIP().toString().c_str());
    } else {
        if (MDNS.begin(kWebHostname)) {
            MDNS.addService("http", "tcp", 80);
        }
        Serial.printf("[WEB] UI at http://%s.local/ (http://%s/)\n", kWebHostname, WiFi.localIP().toString().c_str());
    }
    // Stack must be internal RAM: the task writes to flash (NVS, firmware updates).
    xTaskCreatePinnedToCore(serverTask, "webTask", 8192, nullptr, 1, &serverTaskHandle, 0);
    Serial.printf("[WEB] started | internal heap free %u, largest %u\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

int webServerStackHighWaterMark() {
    return serverTaskHandle ? static_cast<int>(uxTaskGetStackHighWaterMark(serverTaskHandle)) : -1;
}

bool webServerOtaInProgress() {
    return otaInProgress;
}
