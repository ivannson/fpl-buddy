#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <SPD2010.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <miniz.h>
#include <time.h>
#include <cstring>
#include <new>

#include "display_settings.h"
#include "fpl_config.h"
#include "led_ring.h"
#include "wifi_config.h"

LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_22);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_montserrat_48);

static SPD2010Display display;
static SPD2010Touch touch;

static constexpr uint16_t kDisplayWidth = SPD2010_WIDTH;
static constexpr uint16_t kDisplayHeight = SPD2010_HEIGHT;
static constexpr size_t kLvglBufPixels = kDisplayWidth * 40;

static lv_display_t *lvglDisp = nullptr;
static uint8_t *lvglBuf = nullptr;

static uint32_t lastPollMs = 0;
static uint32_t lastWifiRetryMs = 0;
static TaskHandle_t uiTaskHandle = nullptr;
static TaskHandle_t fplTaskHandle = nullptr;
static TaskHandle_t ledTaskHandle = nullptr;

static constexpr int kKitWidth = 110;
static constexpr int kKitHeight = 145;
static constexpr size_t kKitRgb565Bytes = kKitWidth * kKitHeight * 2;
static constexpr size_t kMaxUiEvents = 24;
static constexpr size_t kMaxPopupEvents = 8;
static constexpr size_t kMaxSquadRows = 16;

enum class UiMode {
    Idle,
    Deadline,
    FinalHour,
    Live,
    EventPopup,
    EventsList,
    Squad
};

struct UiEventItem {
    char icon[8] = "";
    char label[24] = "";
    char player[24] = "";
    char team[24] = "";
    int delta = 0;
    int totalBefore = 0;
    int totalAfter = 0;
    bool isGk = false;
    uint32_t epochMs = 0;
};

struct UiSquadRow {
    char player[24] = "";
    char team[24] = "";
    char breakdown[32] = "";
    int points = 0;
    bool hasPlayed = false;
    bool isCaptain = false;
    bool isViceCaptain = false;
    bool isBench = false;
    bool isGk = false;
};

struct UiRuntimeState {
    UiEventItem recentEvents[kMaxUiEvents];
    size_t recentEventCount = 0;
    UiEventItem popupQueue[kMaxPopupEvents];
    size_t popupHead = 0;
    size_t popupTail = 0;
    size_t popupCount = 0;
    UiSquadRow squadRows[kMaxSquadRows];
    size_t squadCount = 0;
    uint32_t eventVersion = 0;
    uint32_t squadVersion = 0;
};

// mbedTLS can only allocate from internal RAM, and a TLS handshake needs ~40 KB of it. Large long-lived buffers
// therefore live in PSRAM (allocated in setup()) rather than as statics, which are placed in internal RAM.
template <typename T> static T *psramNew() {
    void *mem = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return mem ? new (mem) T() : nullptr;
}

static SemaphoreHandle_t uiRuntimeMutex = nullptr;
static UiRuntimeState uiRuntimeState;
static uint8_t *kitImageBuffer = nullptr;  // PSRAM, kKitRgb565Bytes
static lv_image_dsc_t kitImageDsc;

struct UiWidgets {
    lv_obj_t *screenIdle = nullptr;
    lv_obj_t *screenDeadline = nullptr;
    lv_obj_t *screenFinalHour = nullptr;
    lv_obj_t *screenLive = nullptr;
    lv_obj_t *screenPopup = nullptr;
    lv_obj_t *screenEvents = nullptr;
    lv_obj_t *screenSquad = nullptr;

    // Between-gameweeks carousel: screenIdle is the rank page, these are the others. Each body is a
    // full-screen, non-clickable container rebuilt when its data changes (taps fall through to the screen).
    lv_obj_t *screenIdleGraph = nullptr;
    lv_obj_t *screenIdlePrices = nullptr;
    lv_obj_t *screenIdleWatch = nullptr;
    lv_obj_t *screenIdleLeagues = nullptr;
    lv_obj_t *idleGraphBody = nullptr;
    lv_obj_t *idlePricesBody = nullptr;
    lv_obj_t *idleWatchBody = nullptr;
    lv_obj_t *idleLeaguesBody = nullptr;

    // Idle
    lv_obj_t *idleRankArrow = nullptr;
    lv_obj_t *idleRankValue = nullptr;
    lv_obj_t *idleGwPoints = nullptr;
    lv_obj_t *idleTotalPoints = nullptr;

    // Deadline
    lv_obj_t *deadlineLabel = nullptr;
    lv_obj_t *deadlineCountdown = nullptr;
    lv_obj_t *deadlineMeta = nullptr;

    // Final hour
    lv_obj_t *finalArc = nullptr;
    lv_obj_t *finalCountdown = nullptr;

    // Live
    lv_obj_t *liveTitle = nullptr;
    lv_obj_t *liveDot = nullptr;
    lv_obj_t *livePoints = nullptr;
    lv_obj_t *liveRank = nullptr;
    lv_obj_t *liveTickerBtn = nullptr;
    lv_obj_t *liveTickerLabel = nullptr;
    lv_obj_t *liveHoldArc = nullptr;

    // Popup
    lv_obj_t *popupTitle = nullptr;
    lv_obj_t *popupKit = nullptr;
    lv_obj_t *popupPlayer = nullptr;
    lv_obj_t *popupDelta = nullptr;
    lv_obj_t *popupTotal = nullptr;

    // Shared top line / stale
    lv_obj_t *statusLabel = nullptr;

    // Lists
    lv_obj_t *eventsList = nullptr;
    lv_obj_t *squadList = nullptr;
};

static UiWidgets ui;
static UiMode currentMode = UiMode::Idle;

struct SharedUiState {
    int gwPoints = 0;
    bool hasGwPoints = false;
    int overallRank = 0;
    int rankDiff = 0;
    bool hasRankData = false;
    uint32_t statusColor = 0xFFFFFF;
    char statusText[48] = "Booting...";
    char gwStateText[48] = "GW live: ? | next: --";
    int nextGw = 0;
    bool hasNextGw = false;
    time_t nextDeadlineUtc = 0;
    bool hasNextDeadline = false;
    bool isLiveGw = false;
    int currentGw = 0;
    int totalPoints = 0;
    bool hasTotalPoints = false;
    bool isStale = false;
    uint32_t lastApiUpdateMs = 0;
    uint32_t version = 0;
};

static SharedUiState sharedUiState;
static SemaphoreHandle_t sharedUiMutex = nullptr;
static bool timeConfigured = false;
static bool parseIsoUtcToEpoch(const char *iso, time_t &epochOut);
static constexpr bool kUseServerEventBreakdown = (FPL_USE_SERVER_EVENT_BREAKDOWN != 0);

struct TeamPick {
    struct LiveStats {
        int totalPoints = 0;
        int minutes = 0;
        int goalsScored = 0;
        int assists = 0;
        int cleanSheets = 0;
        int goalsConceded = 0;
        int ownGoals = 0;
        int penaltiesSaved = 0;
        int penaltiesMissed = 0;
        int yellowCards = 0;
        int redCards = 0;
        int saves = 0;
        int bonus = 0;
        int defensiveContributions = 0;

        // Per-category points directly from FPL live `explain` payload.
        int brMinutesPts = 0;
        int brGoalsPts = 0;
        int brAssistsPts = 0;
        int brCleanSheetPts = 0;
        int brGoalsConcededPts = 0;
        int brOwnGoalPts = 0;
        int brPenSavedPts = 0;
        int brPenMissedPts = 0;
        int brYellowPts = 0;
        int brRedPts = 0;
        int brSavesPts = 0;
        int brBonusPts = 0;
        int brDefContribPts = 0;
        int brOtherPts = 0;
    };

    int elementId = 0;
    int squadPosition = 0;
    int multiplier = 0;
    bool isCaptain = false;
    bool isViceCaptain = false;
    int elementType = 0;  // 1=GK, 2=DEF, 3=MID, 4=FWD
    int teamId = 0;
    LiveStats live;
    String playerName;
    String positionName;
    char teamShortName[24] = "";
};

struct LastPickState {
    bool valid = false;
    int gw = 0;
    int elementId = 0;
    TeamPick::LiveStats live;
};

struct TeamSnapshot {
    int currentGw = 0;
    int overallRank = 0;
    int overallPoints = 0;
    int rankDiff = 0;
    bool hasRankData = false;
    bool hasGwState = false;
    bool isLiveGw = false;
    int nextGw = 0;
    bool hasDeadline = false;
    time_t deadlineUtc = 0;
    int gwPoints = 0;
    bool hasPlayerMeta = false;
    String activeChip;
    TeamPick picks[16];
    size_t pickCount = 0;
};

struct DemoState {
    bool enabled = false;
    bool seeded = false;
    TeamPick picks[16];
    TeamPick seededPicks[16];
    size_t pickCount = 0;
    int currentGw = 0;
    int nextGw = 0;
    bool hasNextGw = false;
    bool isLiveGw = false;
    bool hasDeadline = false;
    time_t deadlineUtc = 0;
    int gwPoints = 0;
    int seededGwPoints = 0;
    int totalPoints = 0;
    int seededTotalPoints = 0;
    int overallRank = 0;
    int rankDiff = 0;
    bool hasRankData = false;
};

static LastPickState lastPickStates[16];
static DemoState demoState;
static SemaphoreHandle_t demoMutex = nullptr;
enum class ScriptedDemoKind {
    None = 0,
    Notification = 1
};
struct ScriptedDemoState {
    bool active = false;
    ScriptedDemoKind kind = ScriptedDemoKind::None;
    uint8_t stage = 0;
    uint32_t triggerAtMs = 0;
    int baseRank = 0;
};
static ScriptedDemoState scriptedDemoState;
static SemaphoreHandle_t scriptedDemoMutex = nullptr;
static constexpr uint32_t kDemoNotificationDelayMs = 30000U;
static constexpr int32_t kDemoDeadline45mSec = 45 * 60;
static constexpr int32_t kDemoDeadline7mSec = 7 * 60;
static constexpr size_t kSerialLineMax = 192;
static char serialLineBuffer[kSerialLineMax];
static size_t serialLineLen = 0;
static void parseExplainIntoBreakdown(const JsonVariantConst &explainVar, TeamPick::LiveStats &live);
static void pushUiEvent(const UiEventItem &event);
static void updateSharedSquadFromPicks(const TeamPick *picks, size_t pickCount);
static void sanitizeUtf8ToAscii(const char *in, char *out, size_t outLen);
static int computeGwPointsFromPicks(const TeamPick *picks, size_t pickCount);
static bool fetchTeamSnapshot(TeamSnapshot &out);
static void clearUiEvents();
static bool isDemoModeEnabled();
static void publishDemoStateToUi(const DemoState &state);
static bool copyDemoState(DemoState &out);
static void printDemoHelp();
static void printDemoStateSummary(const DemoState &state);
static void printDemoSquad(const DemoState &state);
static bool parseIntToken(const char *token, int &out);
static bool parseBoolToken(const char *token, bool &out);
static void toLowerAscii(char *text);
static size_t splitTokens(char *line, char **tokens, size_t maxTokens);
static TeamPick *findPickBySquadSlot(TeamPick *picks, size_t pickCount, int slot);
static int canonicalPointsForLive(const TeamPick &pick, const TeamPick::LiveStats &live, bool &okOut);
static bool applyDemoEventToPick(TeamPick &pick, const char *eventType, int count, int &pointDeltaOut,
                                 const char *&labelOut);
static bool ensureUkTimeConfigured();
static void serviceScriptedDemo();
static void startNotificationScriptedDemo();
static void startDeadlineScriptedDemo(int32_t secondsToDeadline);
static void squadDemoNotificationBtnCb(lv_event_t *e);
static void squadDemoDeadline45BtnCb(lv_event_t *e);
static void squadDemoDeadline7BtnCb(lv_event_t *e);
static void handleSerialCommandLine(char *line);
static void processSerialInput();

// ---------------------------------------------------------------------------
// HTTP transport
// All requests in a poll share one kept-alive TLS connection and ask for gzip. Compressed bodies are
// inflated into PSRAM with the ESP32-S3 ROM miniz (tinfl), so no extra library is needed.
// ---------------------------------------------------------------------------
static constexpr const char *kFplApiBase = "https://fantasy.premierleague.com/api/";
static constexpr size_t kSmallJsonMaxBytes = 256UL * 1024UL;
static constexpr time_t kMinValidEpoch = 1700000000;  // earlier means the clock is not synced yet

static WiFiClientSecure fplTlsClient;
static HTTPClient fplHttp;
// Guards fplTlsClient, fplHttp and fplCache. Held for a whole poll (fplTask) or `demo seed` (serial task).
static SemaphoreHandle_t fplDataMutex = nullptr;

struct FplHttpStats {
    uint32_t requests = 0;
    uint32_t tlsHandshakes = 0;
    size_t wireBytes = 0;
    size_t jsonBytes = 0;
};
static FplHttpStats fplHttpStats;

static void configureFplHttp() {
    fplTlsClient.setInsecure();
    fplHttp.setConnectTimeout(15000);
    fplHttp.setTimeout(30000);
    fplHttp.useHTTP10(false);
    fplHttp.setReuse(true);
    fplHttp.setUserAgent("fpl-buddy/1.0");
    // HTTPClient ignores addHeader("Accept-Encoding", ...); it must be set here.
    fplHttp.setAcceptEncoding("gzip, identity");
    fplHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    static const char *kHeaderKeys[] = {"Transfer-Encoding"};
    fplHttp.collectHeaders(kHeaderKeys, 1);
}

static void closeFplConnection() {
    fplHttp.end();
    fplTlsClient.stop();
}

// Growable, NUL-terminated PSRAM buffer for response bodies.
class PsramBufferSink : public Stream {
public:
    explicit PsramBufferSink(size_t maxBytes) : maxBytes_(maxBytes) {}
    ~PsramBufferSink() {
        if (buf_) {
            heap_caps_free(buf_);
        }
    }

    bool reserve(size_t bytes) {
        return grow(bytes < maxBytes_ ? bytes : maxBytes_);
    }

    size_t write(uint8_t b) override {
        return write(&b, 1);
    }

    size_t write(const uint8_t *data, size_t size) override {
        if (len_ + size > maxBytes_) {
            overflowed_ = true;
            return 0;
        }
        if (len_ + size > capacity_) {
            size_t target = capacity_ * 2;
            if (target < len_ + size) {
                target = len_ + size;
            }
            if (target > maxBytes_) {
                target = maxBytes_;
            }
            if (!grow(target)) {
                return 0;
            }
        }
        memcpy(buf_ + len_, data, size);
        len_ += size;
        return size;
    }

    int available() override {
        return 0;
    }
    int read() override {
        return -1;
    }
    int peek() override {
        return -1;
    }

    bool overflowed() const {
        return overflowed_;
    }
    size_t length() const {
        return len_;
    }

    // Hands ownership of the NUL-terminated buffer to the caller (free with heap_caps_free).
    char *release() {
        if (!buf_ && !grow(0)) {
            return nullptr;
        }
        buf_[len_] = '\0';
        char *out = buf_;
        buf_ = nullptr;
        capacity_ = 0;
        len_ = 0;
        return out;
    }

private:
    bool grow(size_t capacity) {
        if (buf_ && capacity <= capacity_) {
            return true;
        }
        char *next = static_cast<char *>(heap_caps_realloc(buf_, capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!next) {
            return false;
        }
        buf_ = next;
        capacity_ = capacity;
        return true;
    }

    char *buf_ = nullptr;
    size_t capacity_ = 0;
    size_t len_ = 0;
    size_t maxBytes_;
    bool overflowed_ = false;
};

// Body readers. HTTPClient::writeToStream() is not used: while waiting for TLS data it loops with delay(0)
// (NetworkClientSecure::read returns -1 when nothing is buffered), which never lets IDLE0 run, so a slow
// download on core 0 trips the 5 s task watchdog. These wait with delay(1) instead.
static bool readBodyBytes(NetworkClient &client, PsramBufferSink &sink, size_t len, uint32_t timeoutMs) {
    uint8_t chunk[1024];
    uint32_t lastDataMs = millis();
    while (len > 0) {
        const int avail = client.available();
        if (avail <= 0) {
            if (!client.connected() || millis() - lastDataMs >= timeoutMs) {
                return false;
            }
            delay(1);
            continue;
        }
        size_t want = static_cast<size_t>(avail);
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        if (want > len) {
            want = len;
        }
        const int got = client.read(chunk, want);
        if (got <= 0) {
            delay(1);
            continue;
        }
        if (sink.write(chunk, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
            return false;
        }
        len -= static_cast<size_t>(got);
        lastDataMs = millis();
    }
    return true;
}

static bool readBodyLine(NetworkClient &client, char *line, size_t lineLen, uint32_t timeoutMs) {
    size_t n = 0;
    uint32_t lastDataMs = millis();
    for (;;) {
        if (client.available() <= 0) {
            if (!client.connected() || millis() - lastDataMs >= timeoutMs) {
                return false;
            }
            delay(1);
            continue;
        }
        const int c = client.read();
        if (c < 0) {
            delay(1);
            continue;
        }
        lastDataMs = millis();
        if (c == '\n') {
            if (n > 0 && line[n - 1] == '\r') {
                --n;
            }
            line[n] = '\0';
            return true;
        }
        if (n + 1 < lineLen) {
            line[n++] = static_cast<char>(c);
        }
    }
}

static bool readChunkedBody(NetworkClient &client, PsramBufferSink &sink, uint32_t timeoutMs) {
    char line[64];
    for (;;) {
        if (!readBodyLine(client, line, sizeof(line), timeoutMs)) {
            return false;
        }
        const size_t chunkLen = strtoul(line, nullptr, 16);
        if (chunkLen == 0) {
            // Skip optional trailers up to the terminating empty line.
            do {
                if (!readBodyLine(client, line, sizeof(line), timeoutMs)) {
                    return false;
                }
            } while (line[0] != '\0');
            return true;
        }
        if (!readBodyBytes(client, sink, chunkLen, timeoutMs) || !readBodyLine(client, line, sizeof(line), timeoutMs)) {
            return false;
        }
    }
}

static bool isGzip(const char *data, size_t len) {
    return len >= 18 && static_cast<uint8_t>(data[0]) == 0x1F && static_cast<uint8_t>(data[1]) == 0x8B;
}

static uint32_t readLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Inflates a single gzip member into a new NUL-terminated PSRAM buffer and checks its CRC32.
static bool gunzipToPsram(const char *in, size_t inLen, size_t maxBytes, char *&out, size_t &outLen) {
    out = nullptr;
    outLen = 0;
    const uint8_t *src = static_cast<const uint8_t *>(static_cast<const void *>(in));
    if (!isGzip(in, inLen) || src[2] != 8) {  // 8 = deflate
        return false;
    }

    const uint8_t flags = src[3];
    size_t pos = 10;
    if (flags & 0x04) {  // FEXTRA
        if (pos + 2 > inLen) {
            return false;
        }
        pos += 2 + (static_cast<size_t>(src[pos]) | (static_cast<size_t>(src[pos + 1]) << 8));
    }
    for (uint8_t zeroTerminatedField : {uint8_t{0x08}, uint8_t{0x10}}) {  // FNAME, FCOMMENT
        if (flags & zeroTerminatedField) {
            while (pos < inLen && src[pos]) {
                ++pos;
            }
            ++pos;
        }
    }
    if (flags & 0x02) {  // FHCRC
        pos += 2;
    }
    if (pos + 8 > inLen) {
        return false;
    }

    const uint32_t expectedCrc = readLe32(src + inLen - 8);
    const size_t expectedLen = readLe32(src + inLen - 4);
    if (expectedLen == 0 || expectedLen > maxBytes) {
        Serial.printf("gunzip: inflated size %u exceeds limit %u\n", static_cast<unsigned>(expectedLen),
                      static_cast<unsigned>(maxBytes));
        return false;
    }

    char *buf = static_cast<char *>(heap_caps_malloc(expectedLen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // ~11 KB of Huffman tables; prefer internal RAM for speed.
    tinfl_decompressor *inflator = static_cast<tinfl_decompressor *>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT));
    if (!inflator) {
        inflator = static_cast<tinfl_decompressor *>(
            heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!buf || !inflator) {
        Serial.println("gunzip: allocation failed");
        heap_caps_free(buf);
        heap_caps_free(inflator);
        return false;
    }

    tinfl_init(inflator);
    size_t srcLen = inLen - 8 - pos;
    size_t dstLen = expectedLen;
    const tinfl_status status =
        tinfl_decompress(inflator, src + pos, &srcLen, static_cast<mz_uint8 *>(static_cast<void *>(buf)),
                         static_cast<mz_uint8 *>(static_cast<void *>(buf)), &dstLen,
                         TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(inflator);

    if (status != TINFL_STATUS_DONE || dstLen != expectedLen ||
        mz_crc32(MZ_CRC32_INIT, static_cast<const unsigned char *>(static_cast<const void *>(buf)), dstLen) != expectedCrc) {
        Serial.printf("gunzip failed: status %d, %u/%u bytes\n", static_cast<int>(status), static_cast<unsigned>(dstLen),
                      static_cast<unsigned>(expectedLen));
        heap_caps_free(buf);
        return false;
    }

    buf[dstLen] = '\0';
    out = buf;
    outLen = dstLen;
    return true;
}

// GETs `url` over the shared connection into a PSRAM buffer, inflating gzip bodies.
// On success the caller owns bufferOut (free with heap_caps_free).
static bool fetchUrlToPsramBuffer(const String &url, char *&bufferOut, size_t &lengthOut, size_t maxBytes) {
    bufferOut = nullptr;
    lengthOut = 0;
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    for (int attempt = 1; attempt <= 2; ++attempt) {
        const uint32_t startMs = millis();
        const bool newConnection = !fplTlsClient.connected();
        if (!fplHttp.begin(fplTlsClient, url)) {
            closeFplConnection();
            return false;
        }
        fplHttp.addHeader("Accept", "application/json");

        const int code = fplHttp.GET();
        if (newConnection && code > 0) {
            ++fplHttpStats.tlsHandshakes;
        }
        if (code <= 0) {
            // Also how a kept-alive socket that the server already closed shows up: reconnect and retry once.
            Serial.printf("GET failed [%s]: %s, attempt %d/2\n", url.c_str(), HTTPClient::errorToString(code).c_str(),
                          attempt);
            closeFplConnection();
            continue;
        }
        if (code != HTTP_CODE_OK) {
            Serial.printf("GET failed [%s], HTTP %d\n", url.c_str(), code);
            closeFplConnection();
            return false;
        }

        PsramBufferSink sink(maxBytes);
        const int contentLen = fplHttp.getSize();
        if (!sink.reserve(contentLen > 0 ? static_cast<size_t>(contentLen) : 16384U)) {
            Serial.printf("PSRAM alloc failed [%s]\n", url.c_str());
            closeFplConnection();
            return false;
        }

        static constexpr uint32_t kBodyTimeoutMs = 30000;
        NetworkClient *stream = fplHttp.getStreamPtr();
        bool bodyOk = false;
        if (stream && contentLen > 0) {
            bodyOk = readBodyBytes(*stream, sink, static_cast<size_t>(contentLen), kBodyTimeoutMs);
        } else if (stream && fplHttp.header("Transfer-Encoding").equalsIgnoreCase("chunked")) {
            bodyOk = readChunkedBody(*stream, sink, kBodyTimeoutMs);
        } else {
            Serial.printf("No Content-Length or chunked encoding [%s]\n", url.c_str());
        }
        fplHttp.end();
        if (sink.overflowed()) {
            Serial.printf("Payload too large [%s] (> %u bytes)\n", url.c_str(), static_cast<unsigned>(maxBytes));
            closeFplConnection();
            return false;
        }
        if (!bodyOk || sink.length() == 0) {
            Serial.printf("Body read failed [%s]: %u bytes, attempt %d/2\n", url.c_str(),
                          static_cast<unsigned>(sink.length()), attempt);
            closeFplConnection();
            continue;
        }

        const size_t wireLen = sink.length();
        char *body = sink.release();
        size_t bodyLen = wireLen;
        if (!body) {
            closeFplConnection();
            return false;
        }

        const bool gzipped = isGzip(body, bodyLen);
        if (gzipped) {
            char *inflated = nullptr;
            size_t inflatedLen = 0;
            const bool inflatedOk = gunzipToPsram(body, bodyLen, maxBytes, inflated, inflatedLen);
            heap_caps_free(body);
            if (!inflatedOk) {
                Serial.printf("gunzip failed [%s], attempt %d/2\n", url.c_str(), attempt);
                continue;
            }
            body = inflated;
            bodyLen = inflatedLen;
        }

        ++fplHttpStats.requests;
        fplHttpStats.wireBytes += wireLen;
        fplHttpStats.jsonBytes += bodyLen;
        Serial.printf("GET %s: %u B%s -> %u B json, %lu ms%s\n", url.c_str() + strlen(kFplApiBase),
                      static_cast<unsigned>(wireLen), gzipped ? " gzip" : "", static_cast<unsigned>(bodyLen),
                      static_cast<unsigned long>(millis() - startMs), newConnection ? " (new TLS)" : "");

        bufferOut = body;
        lengthOut = bodyLen;
        return true;
    }

    return false;
}

static bool getJsonDocumentFromPsramUrl(const String &url, DynamicJsonDocument &doc, size_t maxBytes,
                                        JsonDocument *filter = nullptr) {
    for (int attempt = 1; attempt <= 2; ++attempt) {
        char *payload = nullptr;
        size_t payloadLen = 0;
        if (!fetchUrlToPsramBuffer(url, payload, payloadLen, maxBytes)) {
            return false;
        }

        // Parse from const char* so ArduinoJson copies strings into the document; with a mutable char* it
        // would point into `payload`, which is freed below.
        const char *input = payload;
        DeserializationError err;
        if (filter) {
            err = deserializeJson(doc, input, payloadLen, DeserializationOption::Filter(*filter));
        } else {
            err = deserializeJson(doc, input, payloadLen);
        }
        heap_caps_free(payload);

        if (!err) {
            return true;
        }

        Serial.printf("JSON parse error [%s] attempt %d/2: %s\n", url.c_str(), attempt, err.c_str());
        if (attempt == 1 &&
            (err == DeserializationError::IncompleteInput || err == DeserializationError::EmptyInput)) {
            delay(200);
            continue;
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Endpoint fetchers
// ---------------------------------------------------------------------------
static constexpr size_t kMaxMiniLeagues = 24;
static constexpr size_t kMaxGameweeks = 38;

struct MiniLeague {
    int id = 0;
    char name[40] = "";
    int rank = 0;
    int lastRank = 0;  // rank at the end of the previous gameweek; 0 if new
    int size = 0;
};

struct EntrySummary {
    int currentGw = 0;
    int overallRank = 0;
    int overallPoints = 0;
    int previousOverallRank = 0;
    MiniLeague leagues[kMaxMiniLeagues];  // private classic leagues, in API order
    size_t leagueCount = 0;
};

struct RankHistory {
    int gw[kMaxGameweeks] = {};
    int overallRank[kMaxGameweeks] = {};
    size_t count = 0;
};

struct PlayerPrice {
    int elementId = 0;
    char name[24] = "";
    int nowCost = 0;       // tenths of a million
    int projectedPct = 0;  // tenths of a percent toward the next price update; + rise, - fall
    bool locked = false;   // price can't change at the next update
};

struct PriceData {
    bool hasNextUpdate = false;
    time_t nextUpdateUtc = 0;
    PlayerPrice squad[16];
    size_t squadCount = 0;
    PlayerPrice watch[kMaxWatchlist];  // in watchlist order; entries that matched no player are left out
    size_t watchCount = 0;
};

struct FixtureWindow {
    bool hasKickoff = false;
    time_t kickoffUtc = 0;
    bool finished = false;
};

static bool fetchEntrySummary(EntrySummary &out) {
    DynamicJsonDocument filter(512);
    filter["current_event"] = true;
    filter["summary_overall_rank"] = true;
    filter["summary_overall_points"] = true;
    JsonObject leagueFilter = filter["leagues"]["classic"].createNestedObject();
    leagueFilter["id"] = true;
    leagueFilter["name"] = true;
    leagueFilter["short_name"] = true;
    leagueFilter["league_type"] = true;
    leagueFilter["entry_rank"] = true;
    leagueFilter["entry_last_rank"] = true;
    leagueFilter["rank_count"] = true;

    DynamicJsonDocument doc(16384);
    String url = kFplApiBase;
    url += "entry/";
    url += String(FPL_ENTRY_ID);
    url += "/";

    if (!getJsonDocumentFromPsramUrl(url, doc, kSmallJsonMaxBytes, &filter)) {
        return false;
    }

    if (!doc["current_event"].is<int>()) {
        Serial.println("entry response missing current_event");
        return false;
    }

    out.currentGw = doc["current_event"].as<int>();
    out.overallRank = doc["summary_overall_rank"] | 0;
    out.overallPoints = doc["summary_overall_points"] | 0;

    // The system "overall" league carries last gameweek's overall rank, which replaces entry/{id}/history/.
    out.previousOverallRank = 0;
    out.leagueCount = 0;
    for (JsonObject league : doc["leagues"]["classic"].as<JsonArray>()) {
        const char *shortName = league["short_name"] | "";
        if (strcmp(shortName, "overall") == 0) {
            out.previousOverallRank = league["entry_last_rank"] | 0;
        }
        // "x" = private (invitational) league; "s" are system leagues (overall, country, club...).
        const char *leagueType = league["league_type"] | "";
        if (strcmp(leagueType, "x") == 0 && out.leagueCount < kMaxMiniLeagues) {
            MiniLeague &ml = out.leagues[out.leagueCount++];
            ml.id = league["id"] | 0;
            sanitizeUtf8ToAscii(league["name"] | "", ml.name, sizeof(ml.name));
            ml.rank = league["entry_rank"] | 0;
            ml.lastRank = league["entry_last_rank"] | 0;
            ml.size = league["rank_count"] | 0;
        }
    }
    return true;
}

static bool fetchRankHistory(RankHistory &out) {
    DynamicJsonDocument filter(128);
    JsonObject eventFilter = filter["current"].createNestedObject();
    eventFilter["event"] = true;
    eventFilter["overall_rank"] = true;

    DynamicJsonDocument doc(4096);
    String url = kFplApiBase;
    url += "entry/";
    url += String(FPL_ENTRY_ID);
    url += "/history/";

    if (!getJsonDocumentFromPsramUrl(url, doc, kSmallJsonMaxBytes, &filter)) {
        return false;
    }
    JsonArray current = doc["current"].as<JsonArray>();
    if (current.isNull()) {
        Serial.println("history response missing current");
        return false;
    }
    out.count = 0;
    for (JsonObject e : current) {
        const int rank = e["overall_rank"] | 0;
        if (rank <= 0 || out.count >= kMaxGameweeks) {
            continue;
        }
        out.gw[out.count] = e["event"] | 0;
        out.overallRank[out.count] = rank;
        ++out.count;
    }
    return true;
}

static bool fetchPicksForGw(int gw, TeamPick *picks, size_t picksCapacity, size_t &pickCountOut, String &activeChipOut) {
    DynamicJsonDocument filter(512);
    filter["active_chip"] = true;
    JsonObject pickFilter = filter["picks"].createNestedObject();
    pickFilter["element"] = true;
    pickFilter["position"] = true;
    pickFilter["multiplier"] = true;
    pickFilter["is_captain"] = true;
    pickFilter["is_vice_captain"] = true;

    DynamicJsonDocument doc(8192);
    String url = kFplApiBase;
    url += "entry/";
    url += String(FPL_ENTRY_ID);
    url += "/event/";
    url += String(gw);
    url += "/picks/";

    if (!getJsonDocumentFromPsramUrl(url, doc, kSmallJsonMaxBytes, &filter)) {
        return false;
    }

    activeChipOut = doc["active_chip"].is<const char *>() ? doc["active_chip"].as<const char *>() : "none";

    JsonArray picksArray = doc["picks"].as<JsonArray>();
    if (picksArray.isNull()) {
        Serial.println("picks response missing picks array");
        return false;
    }

    pickCountOut = 0;
    for (JsonObject p : picksArray) {
        if (pickCountOut >= picksCapacity) {
            break;
        }
        TeamPick &pick = picks[pickCountOut++];
        pick = TeamPick{};
        pick.elementId = p["element"] | 0;
        pick.squadPosition = p["position"] | 0;
        pick.multiplier = p["multiplier"] | 0;
        pick.isCaptain = p["is_captain"] | false;
        pick.isViceCaptain = p["is_vice_captain"] | false;
    }

    return pickCountOut > 0;
}

static bool fetchFixturesForGw(int gw, FixtureWindow *fixtures, size_t capacity, size_t &countOut) {
    DynamicJsonDocument filter(128);
    JsonObject fixtureFilter = filter.createNestedObject();
    fixtureFilter["kickoff_time"] = true;
    fixtureFilter["finished"] = true;

    DynamicJsonDocument doc(8192);
    String url = kFplApiBase;
    url += "fixtures/?event=";
    url += String(gw);

    if (!getJsonDocumentFromPsramUrl(url, doc, kSmallJsonMaxBytes, &filter)) {
        return false;
    }

    JsonArray items = doc.as<JsonArray>();
    if (items.isNull()) {
        Serial.println("fixtures response is not an array");
        return false;
    }

    countOut = 0;
    for (JsonObject f : items) {
        if (countOut >= capacity) {
            break;
        }
        FixtureWindow &w = fixtures[countOut++];
        w.hasKickoff = parseIsoUtcToEpoch(f["kickoff_time"] | nullptr, w.kickoffUtc);
        w.finished = f["finished"] | false;
    }
    return true;
}

static bool fetchLivePointsForPicks(int gw, TeamPick *picks, size_t pickCount) {
    DynamicJsonDocument filter(512);
    JsonArray elementsFilter = filter.createNestedArray("elements");
    JsonObject elementFilter = elementsFilter.createNestedObject();
    elementFilter["id"] = true;
    elementFilter["stats"]["total_points"] = true;
    elementFilter["stats"]["minutes"] = true;
    elementFilter["stats"]["goals_scored"] = true;
    elementFilter["stats"]["assists"] = true;
    elementFilter["stats"]["clean_sheets"] = true;
    elementFilter["stats"]["goals_conceded"] = true;
    elementFilter["stats"]["own_goals"] = true;
    elementFilter["stats"]["penalties_saved"] = true;
    elementFilter["stats"]["penalties_missed"] = true;
    elementFilter["stats"]["yellow_cards"] = true;
    elementFilter["stats"]["red_cards"] = true;
    elementFilter["stats"]["saves"] = true;
    elementFilter["stats"]["bonus"] = true;
    elementFilter["stats"]["defensive_contributions"] = true;
    elementFilter["stats"]["defensive_contribution"] = true;
    JsonArray explainFilter = elementFilter.createNestedArray("explain");
    JsonObject explainObjFilter = explainFilter.createNestedObject();
    JsonArray explainStatsFilter = explainObjFilter.createNestedArray("stats");
    JsonObject explainStatFilter = explainStatsFilter.createNestedObject();
    explainStatFilter["identifier"] = true;
    explainStatFilter["points"] = true;
    explainStatFilter["value"] = true;

    DynamicJsonDocument doc(FPL_LIVE_JSON_DOC_CAPACITY);
    String url = "https://fantasy.premierleague.com/api/event/";
    url += String(gw);
    url += "/live/";

    if (!getJsonDocumentFromPsramUrl(url, doc, FPL_LIVE_PSRAM_MAX_BYTES, &filter)) {
        return false;
    }

    JsonArray elements = doc["elements"].as<JsonArray>();
    if (elements.isNull()) {
        Serial.println("live response missing elements array");
        return false;
    }

    for (size_t i = 0; i < pickCount; ++i) {
        picks[i].live = TeamPick::LiveStats{};
    }

    for (JsonObject e : elements) {
        const int id = e["id"] | 0;
        JsonObject stats = e["stats"];
        for (size_t i = 0; i < pickCount; ++i) {
            if (picks[i].elementId == id) {
                picks[i].live.totalPoints = stats["total_points"] | 0;
                picks[i].live.minutes = stats["minutes"] | 0;
                picks[i].live.goalsScored = stats["goals_scored"] | 0;
                picks[i].live.assists = stats["assists"] | 0;
                picks[i].live.cleanSheets = stats["clean_sheets"] | 0;
                picks[i].live.goalsConceded = stats["goals_conceded"] | 0;
                picks[i].live.ownGoals = stats["own_goals"] | 0;
                picks[i].live.penaltiesSaved = stats["penalties_saved"] | 0;
                picks[i].live.penaltiesMissed = stats["penalties_missed"] | 0;
                picks[i].live.yellowCards = stats["yellow_cards"] | 0;
                picks[i].live.redCards = stats["red_cards"] | 0;
                picks[i].live.saves = stats["saves"] | 0;
                picks[i].live.bonus = stats["bonus"] | 0;
                picks[i].live.defensiveContributions = stats["defensive_contributions"] | 0;
                if (picks[i].live.defensiveContributions == 0) {
                    picks[i].live.defensiveContributions = stats["defensive_contribution"] | 0;
                }
                parseExplainIntoBreakdown(e["explain"], picks[i].live);
                break;
            }
        }
    }

    return true;
}

#if FPL_ENABLE_NAME_LOOKUP
static void slugifyTeamName(const char *name, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    out[0] = '\0';
    if (!name || !name[0]) {
        return;
    }

    size_t idx = 0;
    bool prevUnderscore = false;
    for (const char *p = name; *p && idx + 1 < outLen; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (isAlphaNum) {
            out[idx++] = c;
            prevUnderscore = false;
        } else if (!prevUnderscore && idx > 0) {
            out[idx++] = '_';
            prevUnderscore = true;
        }
    }

    if (idx > 0 && out[idx - 1] == '_') {
        --idx;
    }
    out[idx] = '\0';
}

static void normalizeKitTeamSlug(char *slug, size_t slugLen) {
    if (!slug || slugLen == 0 || !slug[0]) {
        return;
    }
    struct TeamSlugAlias {
        const char *apiSlug;
        const char *kitSlug;
    };
    static const TeamSlugAlias aliases[] = {
        {"afc_bournemouth", "bournemouth"},
        {"brighton_and_hove_albion", "brighton"},
        {"coventry_city", "coventry"},
        {"hull_city", "hull"},
        {"ipswich_town", "ipswich"},
        {"manchester_city", "man_city"},
        {"manchester_utd", "man_utd"},
        {"manchester_united", "man_utd"},
        {"newcastle_utd", "newcastle"},
        {"newcastle_united", "newcastle"},
        {"nott_m_forest", "nottingham_forest"},
        {"nottm_forest", "nottingham_forest"},
        {"spurs", "tottenham"},
        {"tottenham_hotspur", "tottenham"},
        {"west_ham_united", "west_ham"},
        {"wolverhampton_wanderers", "wolves"},
    };

    for (const TeamSlugAlias &alias : aliases) {
        if (strcmp(slug, alias.apiSlug) == 0) {
            strlcpy(slug, alias.kitSlug, slugLen);
            return;
        }
    }
}

#endif

struct GameweekState {
    int currentGw = 0;
    bool isLiveGw = false;
    int nextGw = 0;
    bool hasDeadline = false;
    time_t deadlineUtc = 0;
};

struct PlayerMeta {
    int elementId = 0;
    int elementType = 0;
    int teamId = 0;
    String playerName;
    String positionName;
    char teamShortName[24] = "";
};

// "49.6" or 49.6 -> 496 (tenths). The price fields in bootstrap-static are sent as strings.
static int jsonTenths(JsonVariantConst v) {
    const float value = v.is<const char *>() ? static_cast<float>(atof(v.as<const char *>())) : v.as<float>();
    return static_cast<int>(value * 10.0f + (value >= 0 ? 0.5f : -0.5f));
}

static bool isAllDigits(const char *text) {
    if (!text || !*text) {
        return false;
    }
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') {
            return false;
        }
    }
    return true;
}

// Watchlist entries match an element ID, or a web name case-insensitively with accents folded ("odegaard").
static bool watchEntryMatches(const char *entry, int elementId, const char *asciiWebName) {
    if (isAllDigits(entry)) {
        return atoi(entry) == elementId;
    }
    return strcasecmp(entry, asciiWebName) == 0;
}

static void fillPlayerPrice(JsonObject e, const char *asciiWebName, time_t nowUtc, PlayerPrice &out) {
    out = PlayerPrice{};
    out.elementId = e["id"] | 0;
    strlcpy(out.name, asciiWebName, sizeof(out.name));
    out.nowCost = e["now_cost"] | 0;
    out.projectedPct = jsonTenths(e["price_change_projections"][0]["projected_percent"]);
    const char *lockedUntil = e["price_change_locked_until"] | nullptr;
    if (lockedUntil) {
        time_t lockedUtc = 0;
        out.locked = !parseIsoUtcToEpoch(lockedUntil, lockedUtc) || nowUtc < kMinValidEpoch || lockedUtc > nowUtc;
    }
}

// One bootstrap-static download for the gameweek flags/deadline, price projections for the squad and the
// watchlist and, when metaOut is given, the names, positions and kit slugs of the squad's players.
static bool fetchBootstrap(GameweekState &stateOut, const TeamPick *picks, size_t pickCount, PlayerMeta *metaOut,
                           size_t &metaCountOut, const DisplaySettings &settings, PriceData &pricesOut) {
    const bool wantMeta = metaOut != nullptr;

    DynamicJsonDocument filter(1536);
    JsonObject eventFilter = filter["events"].createNestedObject();
    eventFilter["id"] = true;
    eventFilter["is_current"] = true;
    eventFilter["is_next"] = true;
    eventFilter["finished"] = true;
    eventFilter["deadline_time"] = true;
    eventFilter["deadline_time_epoch"] = true;
    filter["game_config"]["settings"]["price_change_deadlines"] = true;
    JsonObject elementFilter = filter["elements"].createNestedObject();
    elementFilter["id"] = true;
    elementFilter["web_name"] = true;
    elementFilter["now_cost"] = true;
    elementFilter["price_change_locked_until"] = true;
    elementFilter["price_change_projections"][0]["projected_percent"] = true;
    if (wantMeta) {
        elementFilter["element_type"] = true;
        elementFilter["team"] = true;
        JsonObject typeFilter = filter["element_types"].createNestedObject();
        typeFilter["id"] = true;
        typeFilter["singular_name_short"] = true;
        JsonObject teamFilter = filter["teams"].createNestedObject();
        teamFilter["id"] = true;
        teamFilter["name"] = true;
    }

    DynamicJsonDocument doc(FPL_BOOTSTRAP_JSON_DOC_CAPACITY);
    String url = kFplApiBase;
    url += "bootstrap-static/";
    if (!getJsonDocumentFromPsramUrl(url, doc, FPL_BOOTSTRAP_PSRAM_MAX_BYTES, &filter)) {
        return false;
    }

    JsonArray events = doc["events"].as<JsonArray>();
    if (events.isNull()) {
        Serial.println("bootstrap response missing events");
        return false;
    }

    GameweekState state;
    bool foundCurrent = false;
    bool foundNext = false;
    bool currentFinished = false;
    for (JsonObject e : events) {
        if (e["is_current"] | false) {
            foundCurrent = true;
            state.currentGw = e["id"] | 0;
            currentFinished = e["finished"] | false;
        }
        if (e["is_next"] | false) {
            foundNext = true;
            state.nextGw = e["id"] | 0;
            const char *deadlineIso = e["deadline_time"] | nullptr;
            if (parseIsoUtcToEpoch(deadlineIso, state.deadlineUtc)) {
                state.hasDeadline = true;
            } else {
                const int64_t epoch = e["deadline_time_epoch"] | 0;
                if (epoch > 0) {
                    state.deadlineUtc = static_cast<time_t>(epoch);
                    state.hasDeadline = true;
                    Serial.printf("Using deadline_time_epoch fallback for GW%d: %lld\n", state.nextGw,
                                  static_cast<long long>(epoch));
                } else {
                    Serial.printf("Failed to parse deadline_time for GW%d: %s\n", state.nextGw,
                                  deadlineIso ? deadlineIso : "null");
                }
            }
        }
    }

    if (!foundCurrent && !foundNext) {
        return false;
    }
    // Proxy for live state from bootstrap event flags.
    state.isLiveGw = foundCurrent && !currentFinished;
    stateOut = state;

    // Price updates happen at fixed times (currently 23:00 UTC nightly); take the first one still ahead.
    const time_t nowUtc = time(nullptr);
    PriceData prices;
    for (JsonVariantConst d : doc["game_config"]["settings"]["price_change_deadlines"].as<JsonArrayConst>()) {
        time_t t = 0;
        if (parseIsoUtcToEpoch(d | nullptr, t) && (nowUtc < kMinValidEpoch || t > nowUtc)) {
            prices.nextUpdateUtc = t;
            prices.hasNextUpdate = true;
            break;
        }
    }

    bool watchFound[kMaxWatchlist] = {};
    PlayerPrice watchSlots[kMaxWatchlist];
    for (JsonObject e : doc["elements"].as<JsonArray>()) {
        const int id = e["id"] | 0;
        char asciiName[24];
        sanitizeUtf8ToAscii(e["web_name"] | "", asciiName, sizeof(asciiName));
        for (size_t i = 0; i < pickCount; ++i) {
            if (picks[i].elementId == id && prices.squadCount < 16) {
                fillPlayerPrice(e, asciiName, nowUtc, prices.squad[prices.squadCount++]);
                break;
            }
        }
        for (size_t i = 0; i < settings.watchCount; ++i) {
            if (!watchFound[i] && watchEntryMatches(settings.watchlist[i], id, asciiName)) {
                fillPlayerPrice(e, asciiName, nowUtc, watchSlots[i]);
                watchFound[i] = true;
            }
        }
    }
    for (size_t i = 0; i < settings.watchCount; ++i) {
        if (watchFound[i]) {
            prices.watch[prices.watchCount++] = watchSlots[i];
        } else {
            Serial.printf("[WATCH] no player matches '%s' (use the web name shown on the FPL site, or the ID)\n",
                          settings.watchlist[i]);
        }
    }
    pricesOut = prices;

    if (!wantMeta) {
        return true;
    }

#if FPL_ENABLE_NAME_LOOKUP
    struct TypeName {
        int id = 0;
        char name[8] = "?";
    };
    TypeName typeNames[8];
    size_t typeCount = 0;
    for (JsonObject t : doc["element_types"].as<JsonArray>()) {
        if (typeCount >= (sizeof(typeNames) / sizeof(typeNames[0]))) {
            break;
        }
        typeNames[typeCount].id = t["id"] | 0;
        strlcpy(typeNames[typeCount].name, t["singular_name_short"] | "?", sizeof(typeNames[typeCount].name));
        ++typeCount;
    }

    struct TeamShort {
        int id = 0;
        char slug[24] = "";
    };
    TeamShort teamNames[24];
    size_t teamCount = 0;
    for (JsonObject t : doc["teams"].as<JsonArray>()) {
        if (teamCount >= (sizeof(teamNames) / sizeof(teamNames[0]))) {
            break;
        }
        teamNames[teamCount].id = t["id"] | 0;
        slugifyTeamName(t["name"] | "", teamNames[teamCount].slug, sizeof(teamNames[teamCount].slug));
        normalizeKitTeamSlug(teamNames[teamCount].slug, sizeof(teamNames[teamCount].slug));
        ++teamCount;
    }

    metaCountOut = 0;
    for (JsonObject e : doc["elements"].as<JsonArray>()) {
        const int id = e["id"] | 0;
        for (size_t i = 0; i < pickCount; ++i) {
            if (picks[i].elementId != id) {
                continue;
            }
            PlayerMeta &meta = metaOut[metaCountOut++];
            meta = PlayerMeta{};
            meta.elementId = id;
            meta.playerName = e["web_name"] | "unknown";
            meta.elementType = e["element_type"] | 0;
            meta.teamId = e["team"] | 0;
            meta.positionName = "?";
            for (size_t j = 0; j < typeCount; ++j) {
                if (typeNames[j].id == meta.elementType) {
                    meta.positionName = typeNames[j].name;
                    break;
                }
            }
            for (size_t j = 0; j < teamCount; ++j) {
                if (teamNames[j].id == meta.teamId) {
                    strlcpy(meta.teamShortName, teamNames[j].slug, sizeof(meta.teamShortName));
                    break;
                }
            }
            break;
        }
        if (metaCountOut >= pickCount) {
            break;
        }
    }
#endif
    return true;
}

static const char *pickDisplayName(const TeamPick &pick, char *buf, size_t bufSize) {
    if (pick.playerName.length()) {
        return pick.playerName.c_str();
    }
    snprintf(buf, bufSize, "element %d", pick.elementId);
    return buf;
}

static int goalPointsForElementType(int elementType) {
    if (elementType == 1) {
        return 10;
    }
    if (elementType == 2) {
        return 6;
    }
    if (elementType == 3) {
        return 5;
    }
    if (elementType == 4) {
        return 4;
    }
    return 0;
}

static int cleanSheetPointsForElementType(int elementType) {
    if (elementType == 1 || elementType == 2) {
        return 4;
    }
    if (elementType == 3) {
        return 1;
    }
    return 0;
}

static int defensiveContributionThresholdForElementType(int elementType) {
    if (elementType == 2) {
        return 10;
    }
    if (elementType == 3 || elementType == 4) {
        return 12;
    }
    return 0;
}

static int savesThresholdForElementType(int elementType) {
    if (elementType == 1) {
        return 3;
    }
    return 0;
}

static void addBreakdownPointsByIdentifier(TeamPick::LiveStats &live, const char *identifier, int points) {
    if (!identifier) {
        live.brOtherPts += points;
        return;
    }
    if (strcmp(identifier, "minutes") == 0) {
        live.brMinutesPts += points;
    } else if (strcmp(identifier, "goals_scored") == 0) {
        live.brGoalsPts += points;
    } else if (strcmp(identifier, "assists") == 0) {
        live.brAssistsPts += points;
    } else if (strcmp(identifier, "clean_sheets") == 0) {
        live.brCleanSheetPts += points;
    } else if (strcmp(identifier, "goals_conceded") == 0) {
        live.brGoalsConcededPts += points;
    } else if (strcmp(identifier, "own_goals") == 0) {
        live.brOwnGoalPts += points;
    } else if (strcmp(identifier, "penalties_saved") == 0) {
        live.brPenSavedPts += points;
    } else if (strcmp(identifier, "penalties_missed") == 0) {
        live.brPenMissedPts += points;
    } else if (strcmp(identifier, "yellow_cards") == 0) {
        live.brYellowPts += points;
    } else if (strcmp(identifier, "red_cards") == 0) {
        live.brRedPts += points;
    } else if (strcmp(identifier, "saves") == 0) {
        live.brSavesPts += points;
    } else if (strcmp(identifier, "bonus") == 0) {
        live.brBonusPts += points;
    } else if (strcmp(identifier, "defensive_contribution") == 0 ||
               strcmp(identifier, "defensive_contributions") == 0) {
        live.brDefContribPts += points;
    } else {
        live.brOtherPts += points;
    }
}

static void parseExplainIntoBreakdown(const JsonVariantConst &explainVar, TeamPick::LiveStats &live) {
    if (!explainVar.is<JsonArrayConst>()) {
        return;
    }
    JsonArrayConst explainArr = explainVar.as<JsonArrayConst>();
    for (JsonVariantConst item : explainArr) {
        // Shape A: [{ fixture, stats:[{identifier, points, value}, ...] }, ...]
        if (item.is<JsonObjectConst>()) {
            JsonObjectConst obj = item.as<JsonObjectConst>();
            JsonArrayConst statsArr = obj["stats"].as<JsonArrayConst>();
            for (JsonVariantConst statV : statsArr) {
                JsonObjectConst stat = statV.as<JsonObjectConst>();
                addBreakdownPointsByIdentifier(live, stat["identifier"] | nullptr, stat["points"] | 0);
            }
            continue;
        }

        // Shape B: [[{identifier, points, value}, ...], ...]
        if (item.is<JsonArrayConst>()) {
            JsonArrayConst statsArr = item.as<JsonArrayConst>();
            for (JsonVariantConst statV : statsArr) {
                JsonObjectConst stat = statV.as<JsonObjectConst>();
                addBreakdownPointsByIdentifier(live, stat["identifier"] | nullptr, stat["points"] | 0);
            }
        }
    }
}

static int computeExpectedPointsExcludingBonus(const TeamPick &p, bool &okOut) {
    okOut = false;
    if (p.elementType < 1 || p.elementType > 4) {
        return 0;
    }

    int pts = 0;
    if (p.live.minutes > 0) {
        pts += 1;
    }
    if (p.live.minutes >= 60) {
        pts += 1;
    }

    pts += goalPointsForElementType(p.elementType) * p.live.goalsScored;
    pts += 3 * p.live.assists;
    pts += cleanSheetPointsForElementType(p.elementType) * p.live.cleanSheets;

    if (p.elementType == 1) {
        pts += p.live.saves / 3;
        pts += 5 * p.live.penaltiesSaved;
    }

    if (p.elementType == 1 || p.elementType == 2) {
        pts -= p.live.goalsConceded / 2;
    }

    pts -= 2 * p.live.penaltiesMissed;
    pts -= p.live.yellowCards;
    pts -= 3 * p.live.redCards;
    pts -= 2 * p.live.ownGoals;

    const int dcThreshold = defensiveContributionThresholdForElementType(p.elementType);
    if (dcThreshold > 0) {
        pts += 2 * (p.live.defensiveContributions / dcThreshold);
    }

    okOut = true;
    return pts;
}

static int adjustedLivePointsWithProjectedBonus(const TeamPick &p, bool &projectedBonusAddedOut,
                                                bool &bonusAlreadyIncludedOut) {
    projectedBonusAddedOut = false;
    bonusAlreadyIncludedOut = true;

    if (p.live.bonus <= 0) {
        return p.live.totalPoints;
    }

    bool canScore = false;
    const int noBonus = computeExpectedPointsExcludingBonus(p, canScore);
    if (!canScore) {
        // Unknown element type: prefer raw points to avoid possible double counting.
        return p.live.totalPoints;
    }

    const int withBonus = noBonus + p.live.bonus;
    if (p.live.totalPoints == withBonus) {
        bonusAlreadyIncludedOut = true;
        return p.live.totalPoints;
    }
    if (p.live.totalPoints == noBonus) {
        bonusAlreadyIncludedOut = false;
        projectedBonusAddedOut = true;
        return p.live.totalPoints + p.live.bonus;
    }

    // If raw total is closer to non-bonus score, treat bonus as not yet included.
    const int distNoBonus = abs(p.live.totalPoints - noBonus);
    const int distWithBonus = abs(p.live.totalPoints - withBonus);
    if (distNoBonus <= distWithBonus) {
        bonusAlreadyIncludedOut = false;
        projectedBonusAddedOut = true;
        return p.live.totalPoints + p.live.bonus;
    }

    bonusAlreadyIncludedOut = true;
    return p.live.totalPoints;
}

static void appendBreakdownPart(char *buf, size_t bufLen, bool &firstPart, int pts, const char *label) {
    if (pts == 0 || !label || !buf || bufLen == 0) {
        return;
    }
    char part[64];
    snprintf(part, sizeof(part), "%s%d %s%s", firstPart ? "" : "; ", pts, (abs(pts) == 1) ? "pt" : "pts", label);
    strlcat(buf, part, bufLen);
    firstPart = false;
}

static void formatPointsBreakdown(const TeamPick &p, bool projectedBonusAdded, bool bonusIncluded, int adjustedPoints,
                                  char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    out[0] = '\0';
    bool firstPart = true;
    int explained = 0;

    if (p.live.minutes > 0) {
        appendBreakdownPart(out, outLen, firstPart, +1, " - appearance");
        explained += 1;
    }
    if (p.live.minutes >= 60) {
        appendBreakdownPart(out, outLen, firstPart, +1, " - 60+ mins");
        explained += 1;
    }

    const int goalPts = goalPointsForElementType(p.elementType) * p.live.goalsScored;
    if (goalPts != 0) {
        appendBreakdownPart(out, outLen, firstPart, goalPts, " - goals");
        explained += goalPts;
    }

    const int assistPts = 3 * p.live.assists;
    if (assistPts != 0) {
        appendBreakdownPart(out, outLen, firstPart, assistPts, " - assists");
        explained += assistPts;
    }

    const int csPts = cleanSheetPointsForElementType(p.elementType) * p.live.cleanSheets;
    if (csPts != 0) {
        appendBreakdownPart(out, outLen, firstPart, csPts, " - clean sheet");
        explained += csPts;
    }

    if (p.elementType == 1) {
        const int savePts = p.live.saves / 3;
        if (savePts != 0) {
            appendBreakdownPart(out, outLen, firstPart, savePts, " - saves");
            explained += savePts;
        }
    }

    const int penSavePts = 5 * p.live.penaltiesSaved;
    if (penSavePts != 0) {
        appendBreakdownPart(out, outLen, firstPart, penSavePts, " - pen save");
        explained += penSavePts;
    }

    const int dcThreshold = defensiveContributionThresholdForElementType(p.elementType);
    if (dcThreshold > 0) {
        const int dcPts = 2 * (p.live.defensiveContributions / dcThreshold);
        if (dcPts != 0) {
            appendBreakdownPart(out, outLen, firstPart, dcPts, " - defensive contrib");
            explained += dcPts;
        }
    }

    if (p.live.bonus > 0) {
        if (projectedBonusAdded) {
            appendBreakdownPart(out, outLen, firstPart, p.live.bonus, " - bonus (projected)");
            explained += p.live.bonus;
        } else if (bonusIncluded) {
            appendBreakdownPart(out, outLen, firstPart, p.live.bonus, " - bonus");
            explained += p.live.bonus;
        }
    }

    if (p.elementType == 1 || p.elementType == 2) {
        const int gcPts = -(p.live.goalsConceded / 2);
        if (gcPts != 0) {
            appendBreakdownPart(out, outLen, firstPart, gcPts, " - goals conceded");
            explained += gcPts;
        }
    }

    const int penMissPts = -2 * p.live.penaltiesMissed;
    if (penMissPts != 0) {
        appendBreakdownPart(out, outLen, firstPart, penMissPts, " - pen miss");
        explained += penMissPts;
    }

    const int ycPts = -p.live.yellowCards;
    if (ycPts != 0) {
        appendBreakdownPart(out, outLen, firstPart, ycPts, " - yellow card");
        explained += ycPts;
    }

    const int rcPts = -3 * p.live.redCards;
    if (rcPts != 0) {
        appendBreakdownPart(out, outLen, firstPart, rcPts, " - red card");
        explained += rcPts;
    }

    const int ogPts = -2 * p.live.ownGoals;
    if (ogPts != 0) {
        appendBreakdownPart(out, outLen, firstPart, ogPts, " - own goal");
        explained += ogPts;
    }

    if (firstPart) {
        strlcpy(out, "0 pts - no returns yet", outLen);
        return;
    }

    const int unattributed = adjustedPoints - explained;
    if (unattributed != 0) {
        appendBreakdownPart(out, outLen, firstPart, unattributed, " - other/live adjustments");
    }
}

static const char *iconForEvent(const char *what, int pts) {
    if (!what) {
        return pts >= 0 ? "+" : "-";
    }
    if (strstr(what, "goal") || strstr(what, "GOAL")) {
        return "G";
    }
    if (strstr(what, "assist") || strstr(what, "ASSIST")) {
        return "A";
    }
    if (strstr(what, "clean") || strstr(what, "CLEAN")) {
        return "CS";
    }
    if (strstr(what, "save") || strstr(what, "SAVE")) {
        return "SV";
    }
    if (strstr(what, "yellow") || strstr(what, "YELLOW")) {
        return "YC";
    }
    if (strstr(what, "red") || strstr(what, "RED")) {
        return "RC";
    }
    return pts >= 0 ? "+" : "-";
}

static void notifyEvent(const TeamPick &pick, int pts, const char *what) {
    const char *name = pick.playerName.length() ? pick.playerName.c_str() : "unknown";
    Serial.printf("[FPL EVENT] %s %+d pt%s, %s\n", name, pts, (pts == 1 || pts == -1) ? "" : "s", what);

    UiEventItem event;
    strlcpy(event.icon, iconForEvent(what, pts), sizeof(event.icon));
    sanitizeUtf8ToAscii(what ? what : "event", event.label, sizeof(event.label));
    sanitizeUtf8ToAscii(name, event.player, sizeof(event.player));
    strlcpy(event.team, pick.teamShortName, sizeof(event.team));
    event.delta = pts;
    event.totalAfter = pick.live.totalPoints;
    event.totalBefore = pick.live.totalPoints - pts;
    event.isGk = (pick.elementType == 1);
    event.epochMs = millis();
    pushUiEvent(event);
}

static void detectAndNotifyPointChangesFromBreakdown(int gw, const TeamPick *picks, size_t pickCount) {
    for (size_t i = 0; i < pickCount; ++i) {
        const TeamPick &p = picks[i];
        LastPickState *state = nullptr;
        for (size_t s = 0; s < 16; ++s) {
            if (lastPickStates[s].valid && lastPickStates[s].gw == gw && lastPickStates[s].elementId == p.elementId) {
                state = &lastPickStates[s];
                break;
            }
        }

        // first observation for this player in this GW
        if (!state) {
            for (size_t s = 0; s < 16; ++s) {
                if (!lastPickStates[s].valid || lastPickStates[s].gw != gw) {
                    lastPickStates[s].valid = true;
                    lastPickStates[s].gw = gw;
                    lastPickStates[s].elementId = p.elementId;
                    lastPickStates[s].live = p.live;
                    state = &lastPickStates[s];
                    break;
                }
            }
            continue;
        }

        const TeamPick::LiveStats &prev = state->live;
        const TeamPick::LiveStats &curr = p.live;
        const int pointDelta = curr.totalPoints - prev.totalPoints;
        if (pointDelta == 0) {
            state->live = curr;
            continue;
        }

        int explained = 0;

        auto emitDiff = [&](int prevPts, int currPts, const char *label) {
            const int diff = currPts - prevPts;
            if (diff != 0) {
                notifyEvent(p, diff, label);
                explained += diff;
            }
        };

        const int minutePtsDiff = curr.brMinutesPts - prev.brMinutesPts;
        if (minutePtsDiff > 0) {
            int minutePtsLeft = minutePtsDiff;
            if (prev.minutes < 1 && curr.minutes >= 1 && minutePtsLeft > 0) {
                notifyEvent(p, +1, "PLAYING!");
                explained += 1;
                minutePtsLeft -= 1;
            }
            if (prev.minutes < 60 && curr.minutes >= 60 && minutePtsLeft > 0) {
                notifyEvent(p, +1, "60+ mins!");
                explained += 1;
                minutePtsLeft -= 1;
            }
            if (minutePtsLeft != 0) {
                notifyEvent(p, minutePtsLeft, "60+ mins!");
                explained += minutePtsLeft;
            }
        } else if (minutePtsDiff < 0) {
            notifyEvent(p, minutePtsDiff, "60+ mins!");
            explained += minutePtsDiff;
        }
        emitDiff(prev.brGoalsPts, curr.brGoalsPts, "GOAL!");
        emitDiff(prev.brAssistsPts, curr.brAssistsPts, "ASSIST!");
        emitDiff(prev.brCleanSheetPts, curr.brCleanSheetPts, "CLEAN SHEET!");
        emitDiff(prev.brSavesPts, curr.brSavesPts, "SAVE BONUS!");
        emitDiff(prev.brPenSavedPts, curr.brPenSavedPts, "PEN SAVE!");
        emitDiff(prev.brDefContribPts, curr.brDefContribPts, "DEF CON!");
        emitDiff(prev.brBonusPts, curr.brBonusPts, "BONUS PTS!");
        emitDiff(prev.brGoalsConcededPts, curr.brGoalsConcededPts, "goals against");
        emitDiff(prev.brPenMissedPts, curr.brPenMissedPts, "PEN MISS!");
        emitDiff(prev.brYellowPts, curr.brYellowPts, "YELLOW!");
        emitDiff(prev.brRedPts, curr.brRedPts, "RED!");
        emitDiff(prev.brOwnGoalPts, curr.brOwnGoalPts, "OWN GOAL!");
        emitDiff(prev.brOtherPts, curr.brOtherPts, "other scoring rule");

        if (explained != pointDelta) {
            char nameBuf[24];
            Serial.printf("[FPL EVENT] %s %+d pts total change (breakdown gap %+d)\n",
                          pickDisplayName(p, nameBuf, sizeof(nameBuf)), pointDelta,
                          pointDelta - explained);
        }

        state->live = curr;
    }
}

static void detectAndNotifyPointChanges(int gw, const TeamPick *picks, size_t pickCount) {
    for (size_t i = 0; i < pickCount; ++i) {
        const TeamPick &p = picks[i];
        LastPickState *state = nullptr;
        for (size_t s = 0; s < 16; ++s) {
            if (lastPickStates[s].valid && lastPickStates[s].gw == gw && lastPickStates[s].elementId == p.elementId) {
                state = &lastPickStates[s];
                break;
            }
        }

        // first observation for this player in this GW
        if (!state) {
            for (size_t s = 0; s < 16; ++s) {
                if (!lastPickStates[s].valid || lastPickStates[s].gw != gw) {
                    lastPickStates[s].valid = true;
                    lastPickStates[s].gw = gw;
                    lastPickStates[s].elementId = p.elementId;
                    lastPickStates[s].live = p.live;
                    state = &lastPickStates[s];
                    break;
                }
            }
            continue;
        }

        const TeamPick::LiveStats &prev = state->live;
        const TeamPick::LiveStats &curr = p.live;
        const int pointDelta = curr.totalPoints - prev.totalPoints;
        if (pointDelta == 0) {
            state->live = curr;
            continue;
        }

        int explained = 0;

        if (prev.minutes < 1 && curr.minutes >= 1) {
            notifyEvent(p, +1, "PLAYING!");
            explained += 1;
        }
        if (prev.minutes < 60 && curr.minutes >= 60) {
            notifyEvent(p, +1, "60+ mins!");
            explained += 1;
        }

        const int goalDiff = curr.goalsScored - prev.goalsScored;
        if (goalDiff > 0) {
            const int pts = goalPointsForElementType(p.elementType) * goalDiff;
            notifyEvent(p, pts, "GOAL!");
            explained += pts;
        }

        const int assistDiff = curr.assists - prev.assists;
        if (assistDiff > 0) {
            const int pts = 3 * assistDiff;
            notifyEvent(p, pts, "ASSIST!");
            explained += pts;
        }

        const int csDiff = curr.cleanSheets - prev.cleanSheets;
        if (csDiff > 0) {
            const int pts = cleanSheetPointsForElementType(p.elementType) * csDiff;
            if (pts != 0) {
                notifyEvent(p, pts, "CLEAN SHEET!");
                explained += pts;
            }
        }

        const int savesThreshold = savesThresholdForElementType(p.elementType);
        if (savesThreshold > 0) {
            const int saveChunksPrev = prev.saves / savesThreshold;
            const int saveChunksCurr = curr.saves / savesThreshold;
            const int chunkDiff = saveChunksCurr - saveChunksPrev;
            if (chunkDiff > 0) {
                notifyEvent(p, chunkDiff, "SAVE BONUS!");
                explained += chunkDiff;
            }
        }

        const int psDiff = curr.penaltiesSaved - prev.penaltiesSaved;
        if (psDiff > 0) {
            const int pts = 5 * psDiff;
            notifyEvent(p, pts, "PEN SAVE!");
            explained += pts;
        }

        const int dcThreshold = defensiveContributionThresholdForElementType(p.elementType);
        if (dcThreshold > 0) {
            const int dcChunksPrev = prev.defensiveContributions / dcThreshold;
            const int dcChunksCurr = curr.defensiveContributions / dcThreshold;
            const int chunkDiff = dcChunksCurr - dcChunksPrev;
            if (chunkDiff > 0) {
                const int pts = 2 * chunkDiff;
                notifyEvent(p, pts, "DEF CON!");
                explained += pts;
            }
        }

        const int bonusDiff = curr.bonus - prev.bonus;
        if (bonusDiff > 0) {
            notifyEvent(p, bonusDiff, "BONUS PTS!");
            explained += bonusDiff;
        }

        if (p.elementType == 1 || p.elementType == 2) {
            const int gcChunksPrev = prev.goalsConceded / 2;
            const int gcChunksCurr = curr.goalsConceded / 2;
            const int gcChunkDiff = gcChunksCurr - gcChunksPrev;
            if (gcChunkDiff > 0) {
                notifyEvent(p, -gcChunkDiff, "goals against");
                explained -= gcChunkDiff;
            }
        }

        const int pmDiff = curr.penaltiesMissed - prev.penaltiesMissed;
        if (pmDiff > 0) {
            const int pts = -2 * pmDiff;
            notifyEvent(p, pts, "PEN MISS!");
            explained += pts;
        }

        const int ycDiff = curr.yellowCards - prev.yellowCards;
        if (ycDiff > 0) {
            const int pts = -ycDiff;
            notifyEvent(p, pts, "YELLOW!");
            explained += pts;
        }

        const int rcDiff = curr.redCards - prev.redCards;
        if (rcDiff > 0) {
            const int pts = -3 * rcDiff;
            notifyEvent(p, pts, "RED!");
            explained += pts;
        }

        const int ogDiff = curr.ownGoals - prev.ownGoals;
        if (ogDiff > 0) {
            const int pts = -2 * ogDiff;
            notifyEvent(p, pts, "OWN GOAL!");
            explained += pts;
        }

        if (explained != pointDelta) {
            char nameBuf[24];
            Serial.printf("[FPL EVENT] %s %+d pts total change (unattributed %+d)\n",
                          pickDisplayName(p, nameBuf, sizeof(nameBuf)), pointDelta,
                          pointDelta - explained);
        }

        state->live = curr;
    }
}

static int computeGwPointsFromPicks(const TeamPick *picks, size_t pickCount) {
    int computedGwPoints = 0;
    for (size_t i = 0; i < pickCount; ++i) {
        bool projectedBonusAdded = false;
        bool bonusIncluded = false;
        const int adjusted = adjustedLivePointsWithProjectedBonus(picks[i], projectedBonusAdded, bonusIncluded);
        computedGwPoints += adjusted * picks[i].multiplier;
    }
    return computedGwPoints;
}

// ---------------------------------------------------------------------------
// Poll cache + scheduling
// Each endpoint is refetched only when its data can have changed (intervals in fpl_config.h):
//   entry/{id}/              every poll (current GW, total points, overall rank and last rank, mini leagues)
//   entry/{id}/event/picks/  new GW or every FPL_PICKS_REFRESH_MS (auto-subs change multipliers)
//   bootstrap-static/        first poll, GW change, deadline passed, unknown players, price update passed,
//                            watchlist changed, or FPL_BOOTSTRAP_REFRESH_MS
//   fixtures/?event={gw}     new GW or every FPL_FIXTURES_REFRESH_MS
//   event/{gw}/live/         while a match is in play, when a fixture is newly finished, or FPL_LIVE_IDLE_REFRESH_MS
//   entry/{id}/history/      new GW or every FPL_HISTORY_REFRESH_MS (rank graph)
// Polls run every FPL_POLL_INTERVAL_MS while a match is in play, otherwise every FPL_IDLE_POLL_INTERVAL_MS
// (woken early for the next kickoff or deadline).
// ---------------------------------------------------------------------------
static constexpr size_t kMaxFixtures = 24;
// A fixture counts as in play from kickoff until FPL marks it finished; capped in case it was abandoned.
static constexpr time_t kFixtureInPlayMaxSec = 6 * 3600;
// bootstrap-static is served with Cache-Control: max-age=300, so refetching sooner returns the same copy.
static constexpr uint32_t kBootstrapMinSpacingMs = 5UL * 60UL * 1000UL;
static constexpr time_t kPostDeadlineWakeSec = 60;
// Prices are recalculated at each price update time; refetch bootstrap once this long after it.
static constexpr time_t kPriceUpdateSettleSec = 10 * 60;

struct FplDataCache {
    bool hasEntry = false;
    EntrySummary entry;

    bool hasGwState = false;
    GameweekState gwState;
    uint32_t bootstrapFetchedMs = 0;
    time_t bootstrapFetchedUtc = 0;
    PlayerMeta meta[16];
    size_t metaCount = 0;
    bool hasPlayerMeta = false;
    bool hasPrices = false;
    PriceData prices;
    uint32_t pricesWatchlistVersion = 0;  // DisplaySettings::watchlistVersion the prices were parsed with

    int picksGw = 0;
    uint32_t picksFetchedMs = 0;
    TeamPick picks[16];
    size_t pickCount = 0;
    String activeChip;

    int fixturesGw = 0;
    uint32_t fixturesFetchedMs = 0;
    FixtureWindow fixtures[kMaxFixtures];
    size_t fixtureCount = 0;

    int liveGw = 0;
    uint32_t liveFetchedMs = 0;
    size_t liveFinishedFixtures = 0;

    int historyGw = 0;
    uint32_t historyFetchedMs = 0;
    RankHistory history;
};
static FplDataCache *fplCache = nullptr;  // PSRAM; guarded by fplDataMutex

struct FplPollResult {
    bool liveFetched = false;
    bool matchInPlay = false;
    uint32_t nextPollDelayMs = FPL_POLL_INTERVAL_MS;
};

static bool elapsedAtLeast(uint32_t sinceMs, uint32_t intervalMs) {
    return sinceMs == 0 || millis() - sinceMs >= intervalMs;
}

#if FPL_ENABLE_NAME_LOOKUP
// Copies cached names/positions/teams onto picks; false if any pick has no cached meta.
static bool applyPlayerMeta(TeamPick *picks, size_t pickCount, const PlayerMeta *meta, size_t metaCount) {
    bool allFound = true;
    for (size_t i = 0; i < pickCount; ++i) {
        bool found = false;
        for (size_t j = 0; j < metaCount; ++j) {
            if (meta[j].elementId != picks[i].elementId) {
                continue;
            }
            picks[i].playerName = meta[j].playerName;
            picks[i].positionName = meta[j].positionName;
            picks[i].elementType = meta[j].elementType;
            picks[i].teamId = meta[j].teamId;
            strlcpy(picks[i].teamShortName, meta[j].teamShortName, sizeof(picks[i].teamShortName));
            found = true;
            break;
        }
        allFound = allFound && found;
    }
    return allFound;
}
#endif

// Copies live stats from the previous picks of the same GW; false if a player is new to the squad.
static bool carryOverLiveStats(TeamPick *picks, size_t pickCount, const TeamPick *previous, size_t previousCount) {
    bool allFound = true;
    for (size_t i = 0; i < pickCount; ++i) {
        bool found = false;
        for (size_t j = 0; j < previousCount; ++j) {
            if (previous[j].elementId == picks[i].elementId) {
                picks[i].live = previous[j].live;
                found = true;
                break;
            }
        }
        allFound = allFound && found;
    }
    return allFound;
}

static uint32_t computeNextPollDelayMs(const FplDataCache &c, bool matchInPlay, bool clockValid, time_t nowUtc) {
    if (matchInPlay || !clockValid) {
        return FPL_POLL_INTERVAL_MS;
    }

    uint64_t delayMs = FPL_IDLE_POLL_INTERVAL_MS;
    auto wakeAt = [&](time_t eventUtc) {
        if (eventUtc <= nowUtc) {
            return;
        }
        const uint64_t untilMs = static_cast<uint64_t>(eventUtc - nowUtc) * 1000ULL;
        if (untilMs < delayMs) {
            delayMs = untilMs;
        }
    };
    if (c.fixturesGw == c.entry.currentGw) {
        for (size_t i = 0; i < c.fixtureCount; ++i) {
            if (c.fixtures[i].hasKickoff && !c.fixtures[i].finished) {
                wakeAt(c.fixtures[i].kickoffUtc);
            }
        }
    }
    if (c.hasGwState && c.gwState.hasDeadline) {
        wakeAt(c.gwState.deadlineUtc + kPostDeadlineWakeSec);
    }
    if (c.hasPrices && c.prices.hasNextUpdate) {
        wakeAt(c.prices.nextUpdateUtc + kPriceUpdateSettleSec);
    }

    if (delayMs < FPL_POLL_INTERVAL_MS) {
        delayMs = FPL_POLL_INTERVAL_MS;
    }
    return static_cast<uint32_t>(delayMs);
}

static bool refreshFplDataSteps(FplDataCache &c, FplPollResult &result) {
    const time_t nowUtc = time(nullptr);
    const bool clockValid = nowUtc >= kMinValidEpoch;

    // 1) entry/{id}/ every poll (also carries mini-league ranks).
    // The fetchers below only write their output after a successful parse, so they fill the cache directly.
    if (!fetchEntrySummary(c.entry)) {
        return false;
    }
    c.hasEntry = true;
    const int gw = c.entry.currentGw;

    // 2) picks: change at the deadline and when auto-subs are applied.
    bool squadChanged = false;
    if (c.picksGw != gw || elapsedAtLeast(c.picksFetchedMs, FPL_PICKS_REFRESH_MS)) {
        static TeamPick fresh[16];  // static: keeps ~3 KB off the task stack (fplDataMutex is held)
        size_t freshCount = 0;
        String chip;
        if (fetchPicksForGw(gw, fresh, 16, freshCount, chip)) {
            squadChanged = !(c.picksGw == gw && carryOverLiveStats(fresh, freshCount, c.picks, c.pickCount));
            for (size_t i = 0; i < freshCount; ++i) {
                c.picks[i] = fresh[i];
            }
            c.pickCount = freshCount;
            c.activeChip = chip;
            c.picksGw = gw;
            c.picksFetchedMs = millis();
        } else if (c.picksGw != gw) {
            return false;
        }
    }
#if FPL_ENABLE_NAME_LOOKUP
    c.hasPlayerMeta = applyPlayerMeta(c.picks, c.pickCount, c.meta, c.metaCount);
#endif

    // 3) bootstrap-static (1.7 MB, ~160 KB gzipped).
    const bool deadlinePassed = c.hasGwState && c.gwState.hasDeadline && clockValid &&
                                nowUtc >= c.gwState.deadlineUtc && c.bootstrapFetchedUtc < c.gwState.deadlineUtc;
    bool needMeta = false;
#if FPL_ENABLE_NAME_LOOKUP
    needMeta = !c.hasPlayerMeta;
#endif
    const bool gwMismatch = c.hasGwState && c.gwState.currentGw != gw;
    const bool pricesUpdated = c.hasPrices && c.prices.hasNextUpdate && clockValid &&
                               nowUtc >= c.prices.nextUpdateUtc + kPriceUpdateSettleSec;
    static DisplaySettings settings;  // static: keeps it off the task stack
    displaySettingsGet(settings);
    const bool watchlistChanged = c.pricesWatchlistVersion != settings.watchlistVersion;
    if (!c.hasGwState || watchlistChanged || elapsedAtLeast(c.bootstrapFetchedMs, FPL_BOOTSTRAP_REFRESH_MS) ||
        ((needMeta || gwMismatch || deadlinePassed || pricesUpdated) &&
         elapsedAtLeast(c.bootstrapFetchedMs, kBootstrapMinSpacingMs))) {
        GameweekState state;
        size_t metaCount = 0;
        PlayerMeta *metaOut = nullptr;
#if FPL_ENABLE_NAME_LOOKUP
        metaOut = c.meta;
#endif
        if (fetchBootstrap(state, c.picks, c.pickCount, metaOut, metaCount, settings, c.prices)) {
            c.gwState = state;
            c.hasGwState = true;
            c.bootstrapFetchedMs = millis();
            c.bootstrapFetchedUtc = clockValid ? nowUtc : 0;
            c.hasPrices = true;
            c.pricesWatchlistVersion = settings.watchlistVersion;
#if FPL_ENABLE_NAME_LOOKUP
            c.metaCount = metaCount;
            c.hasPlayerMeta = applyPlayerMeta(c.picks, c.pickCount, c.meta, c.metaCount);
#endif
        }
    }

    // 4) fixtures: kickoff times and finished flags decide whether live data can be changing.
    if (c.fixturesGw != gw || elapsedAtLeast(c.fixturesFetchedMs, FPL_FIXTURES_REFRESH_MS)) {
        size_t count = 0;
        if (fetchFixturesForGw(gw, c.fixtures, kMaxFixtures, count)) {
            c.fixtureCount = count;
            c.fixturesGw = gw;
            c.fixturesFetchedMs = millis();
        }
    }

    // Without fixtures or a synced clock, fall back to treating the GW as in play (fetch live every poll).
    const bool haveFixtures = c.fixturesGw == gw;
    bool matchInPlay = !haveFixtures || !clockValid;
    size_t finishedFixtures = 0;
    if (haveFixtures) {
        for (size_t i = 0; i < c.fixtureCount; ++i) {
            const FixtureWindow &f = c.fixtures[i];
            if (f.finished) {
                ++finishedFixtures;
            } else if (clockValid && f.hasKickoff && nowUtc >= f.kickoffUtc &&
                       nowUtc - f.kickoffUtc < kFixtureInPlayMaxSec) {
                matchInPlay = true;
            }
        }
    }
    result.matchInPlay = matchInPlay;

    // 5) event/{gw}/live/. A newly finished fixture means confirmed bonus, so fetch once more after it.
    if (c.liveGw != gw || squadChanged || matchInPlay || finishedFixtures != c.liveFinishedFixtures ||
        elapsedAtLeast(c.liveFetchedMs, FPL_LIVE_IDLE_REFRESH_MS)) {
        if (fetchLivePointsForPicks(gw, c.picks, c.pickCount)) {
            c.liveGw = gw;
            c.liveFetchedMs = millis();
            c.liveFinishedFixtures = finishedFixtures;
            result.liveFetched = true;
        } else if (c.liveGw != gw) {
            return false;
        }
    }

    // 6) entry/{id}/history/ for the rank graph: final ranks only change after a GW, so hourly is enough.
    if (c.historyGw != gw || elapsedAtLeast(c.historyFetchedMs, FPL_HISTORY_REFRESH_MS)) {
        if (fetchRankHistory(c.history)) {
            c.historyGw = gw;
            c.historyFetchedMs = millis();
        }
    }

    result.nextPollDelayMs = computeNextPollDelayMs(c, matchInPlay, clockValid, nowUtc);
    return true;
}

// Refreshes fplCache over one kept-alive connection. Caller must hold fplDataMutex.
static bool refreshFplData(FplPollResult &result) {
    result = FplPollResult{};
    fplHttpStats = FplHttpStats{};
    const uint32_t startMs = millis();

    const bool ok = refreshFplDataSteps(*fplCache, result);
    closeFplConnection();

    Serial.printf("[FPL poll] %s in %lu ms | %u requests, %u TLS handshakes, %u B wire, %u B json | "
                  "match in play: %s | next poll in %lu s | internal heap free %u, largest %u\n",
                  ok ? "ok" : "FAILED", static_cast<unsigned long>(millis() - startMs),
                  static_cast<unsigned>(fplHttpStats.requests), static_cast<unsigned>(fplHttpStats.tlsHandshakes),
                  static_cast<unsigned>(fplHttpStats.wireBytes), static_cast<unsigned>(fplHttpStats.jsonBytes),
                  result.matchInPlay ? "yes" : "no",
                  static_cast<unsigned long>((ok ? result.nextPollDelayMs : FPL_POLL_INTERVAL_MS) / 1000UL),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return ok;
}

static void fillSnapshotFromCache(const FplDataCache &c, TeamSnapshot &out) {
    out.currentGw = c.entry.currentGw;
    out.overallRank = c.entry.overallRank;
    out.overallPoints = c.entry.overallPoints;
    out.hasRankData = c.entry.overallRank > 0 && c.entry.previousOverallRank > 0;
    // Positive means improvement (rank number got smaller).
    out.rankDiff = out.hasRankData ? c.entry.previousOverallRank - c.entry.overallRank : 0;
    out.hasGwState = c.hasGwState;
    out.isLiveGw = c.gwState.isLiveGw;
    out.nextGw = c.gwState.nextGw;
    out.hasDeadline = c.gwState.hasDeadline;
    out.deadlineUtc = c.gwState.deadlineUtc;
    out.activeChip = c.activeChip;
    out.hasPlayerMeta = c.hasPlayerMeta;
    out.pickCount = c.pickCount;
    for (size_t i = 0; i < c.pickCount; ++i) {
        out.picks[i] = c.picks[i];
    }
    out.gwPoints = computeGwPointsFromPicks(out.picks, out.pickCount);
}

// Data for the between-gameweeks screens. fplTask publishes it after each successful poll; uiTask copies it
// when `version` changes.
struct IdleScreensData {
    bool hasHistory = false;
    RankHistory history;
    bool hasPrices = false;
    PriceData prices;
    bool hasLeagues = false;
    MiniLeague leagues[kMaxMiniLeagues];
    size_t leagueCount = 0;
    uint32_t version = 0;
};
static IdleScreensData *idleScreensData = nullptr;  // PSRAM
static SemaphoreHandle_t idleScreensMutex = nullptr;
// Set by serial commands that need fresh data (e.g. a watchlist change); fplTask polls on its next loop.
static volatile bool fplPollRequested = false;

static void publishIdleScreensFromCache(const FplDataCache &c) {
    if (!idleScreensMutex || xSemaphoreTake(idleScreensMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    IdleScreensData &d = *idleScreensData;
    d.hasHistory = c.historyGw > 0;
    d.history = c.history;
    d.hasPrices = c.hasPrices;
    d.prices = c.prices;
    // Prices were parsed for the squad at the last bootstrap fetch; drop players transferred out since.
    d.prices.squadCount = 0;
    for (size_t i = 0; i < c.prices.squadCount; ++i) {
        for (size_t j = 0; j < c.pickCount; ++j) {
            if (c.picks[j].elementId == c.prices.squad[i].elementId) {
                d.prices.squad[d.prices.squadCount++] = c.prices.squad[i];
                break;
            }
        }
    }
    d.hasLeagues = c.hasEntry;
    d.leagueCount = c.entry.leagueCount;
    for (size_t i = 0; i < c.entry.leagueCount; ++i) {
        d.leagues[i] = c.entry.leagues[i];
    }
    d.version++;
    xSemaphoreGive(idleScreensMutex);
}

// Copies the shared data if its version differs from knownVersion.
static bool copyIdleScreensDataIfChanged(IdleScreensData &out, uint32_t knownVersion) {
    if (!idleScreensMutex || xSemaphoreTake(idleScreensMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const bool changed = idleScreensData->version != knownVersion;
    if (changed) {
        out = *idleScreensData;
    }
    xSemaphoreGive(idleScreensMutex);
    return changed;
}

// Used by `demo seed`: runs a normal cache-aware refresh and copies the result.
static bool fetchTeamSnapshot(TeamSnapshot &out) {
    if (!fplDataMutex || xSemaphoreTake(fplDataMutex, pdMS_TO_TICKS(120000)) != pdTRUE) {
        return false;
    }
    FplPollResult result;
    const bool ok = refreshFplData(result);
    if (ok) {
        fillSnapshotFromCache(*fplCache, out);
        publishIdleScreensFromCache(*fplCache);
    }
    xSemaphoreGive(fplDataMutex);
    return ok;
}

static void printTeamSnapshot(TeamSnapshot &snapshot) {
    Serial.println("\n=== FPL Team Snapshot ===");
    Serial.printf("Entry ID: %d | GW: %d | GW points: %d\n", FPL_ENTRY_ID, snapshot.currentGw, snapshot.gwPoints);
    if (snapshot.overallRank > 0) {
        Serial.printf("Overall rank: %d\n", snapshot.overallRank);
    }
    Serial.printf("Active chip: %s\n", snapshot.activeChip.c_str());
#if FPL_ENABLE_NAME_LOOKUP
    Serial.printf("Name lookup: %s\n", snapshot.hasPlayerMeta ? "ok" : "fallback-id-only");
#else
    Serial.println("Name lookup: disabled");
#endif
    Serial.println("Players:");

    for (size_t i = 0; i < snapshot.pickCount; ++i) {
        TeamPick &p = snapshot.picks[i];
        const char *slot = (p.squadPosition <= 11) ? "XI" : "BENCH";
        bool projectedBonusAdded = false;
        bool bonusIncluded = false;
        const int currPoints = adjustedLivePointsWithProjectedBonus(p, projectedBonusAdded, bonusIncluded);
        const int effectivePoints = currPoints * p.multiplier;
        const bool showBonusState = p.live.bonus > 0;
        const char *bonusState = projectedBonusAdded ? "proj" : (bonusIncluded ? "in" : "unk");
        char breakdown[256];
        formatPointsBreakdown(p, projectedBonusAdded, bonusIncluded, currPoints, breakdown, sizeof(breakdown));
        if (snapshot.hasPlayerMeta) {
            Serial.printf("  [%2d] %-5s | %-15s | %-3s | curr:%2d | element:%4d | mult:%d%s%s | eff:%2d",
                          p.squadPosition, slot,
                          p.playerName.length() ? p.playerName.c_str() : "unknown",
                          p.positionName.length() ? p.positionName.c_str() : "?", currPoints, p.elementId,
                          p.multiplier, p.isCaptain ? " C" : "", p.isViceCaptain ? " VC" : "", effectivePoints);
        } else {
            Serial.printf("  [%2d] %-5s | curr:%2d | element:%4d | mult:%d%s%s | eff:%2d", p.squadPosition, slot,
                          currPoints, p.elementId, p.multiplier, p.isCaptain ? " C" : "",
                          p.isViceCaptain ? " VC" : "", effectivePoints);
        }
        if (showBonusState) {
            Serial.printf(" | bonus:%d(%s)", p.live.bonus, bonusState);
        }
        Serial.println();
        Serial.printf("       breakdown: %s\n", breakdown);
    }
    Serial.println("=========================\n");
}

static void lvglFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int x1 = area->x1;
    const int y1 = area->y1;
    const int x2 = area->x2;
    const int y2 = area->y2;
    const int w = x2 - x1 + 1;
    const int h = y2 - y1 + 1;

    display.drawBitmap(x1, y1, w, h, px_map);
    lv_display_flush_ready(disp);
}

static uint32_t lvglTickCb(void) {
    return millis();
}

static void lvglTouchCb(lv_indev_t *, lv_indev_data_t *data) {
    int x = 0;
    int y = 0;
    if (touch.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void formatNumberWithCommas(int value, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    char raw[24];
    snprintf(raw, sizeof(raw), "%d", value);
    const int rawLen = static_cast<int>(strlen(raw));
    if (rawLen <= 3) {
        strlcpy(out, raw, outLen);
        return;
    }

    char rev[32];
    int idx = 0;
    int digits = 0;
    for (int i = rawLen - 1; i >= 0; --i) {
        rev[idx++] = raw[i];
        ++digits;
        if (digits == 3 && i > 0) {
            rev[idx++] = ',';
            digits = 0;
        }
    }
    int outIdx = 0;
    for (int i = idx - 1; i >= 0 && static_cast<size_t>(outIdx + 1) < outLen; --i) {
        out[outIdx++] = rev[i];
    }
    out[outIdx] = '\0';
}

static constexpr uint32_t kColorBgDeep = 0x1A0533;
static constexpr uint32_t kColorBgSurface = 0x2D1B4E;
static constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
static constexpr uint32_t kColorTextSecondary = 0xB0A0C0;
static constexpr uint32_t kColorAccentGreen = 0x00FF87;
static constexpr uint32_t kColorAccentRed = 0xFF2882;
static constexpr uint32_t kColorAccentAmber = 0xFFC107;
static constexpr uint32_t kColorAccentCyan = 0x00E5FF;
static constexpr uint32_t kColorButtonPurple = 0x6A3DFF;

static const lv_font_t *kFontHero = &lv_font_montserrat_48;
static const lv_font_t *kFontLarge = &lv_font_montserrat_32;
static const lv_font_t *kFontBody = &lv_font_montserrat_20;
static const lv_font_t *kFontCaption = &lv_font_montserrat_18;
static const lv_font_t *kFontMicro = &lv_font_montserrat_14;

static uint32_t popupHideAtMs = 0;
static constexpr uint32_t kPopupDisplayDurationMs = 4000U;
static uint32_t lastTickerRotateMs = 0;
static uint32_t lastDeadlineBlinkMs = 0;
static bool deadlineColonVisible = true;
static size_t tickerEventIndex = 0;
static uint32_t holdStartMs = 0;
static bool holdTriggered = false;
static uint32_t renderedEventsVersion = 0;
static uint32_t renderedSquadVersion = 0;

enum class IdlePage : uint8_t { Rank = 0, RankGraph, Prices, Watchlist, Leagues };
static constexpr uint8_t kIdlePageCount = 5;
static IdlePage idlePage = IdlePage::Rank;
static size_t idleLeaguePage = 0;  // the leagues screen pages through leagues before moving on
static uint32_t idlePageShownMs = 0;

static lv_obj_t *idlePageScreen(IdlePage page) {
    switch (page) {
        case IdlePage::Rank: return ui.screenIdle;
        case IdlePage::RankGraph: return ui.screenIdleGraph;
        case IdlePage::Prices: return ui.screenIdlePrices;
        case IdlePage::Watchlist: return ui.screenIdleWatch;
        case IdlePage::Leagues: return ui.screenIdleLeagues;
    }
    return ui.screenIdle;
}

static lv_obj_t *modeToScreen(UiMode mode) {
    switch (mode) {
        case UiMode::Idle: return idlePageScreen(idlePage);
        case UiMode::Deadline: return ui.screenDeadline;
        case UiMode::FinalHour: return ui.screenFinalHour;
        case UiMode::Live: return ui.screenLive;
        case UiMode::EventPopup: return ui.screenPopup;
        case UiMode::EventsList: return ui.screenEvents;
        case UiMode::Squad: return ui.screenSquad;
    }
    return ui.screenIdle;
}

static void loadMode(UiMode mode, lv_screen_load_anim_t anim) {
    if (currentMode == mode) {
        return;
    }
    if (mode == UiMode::Idle) {
        // Entering the between-gameweeks carousel always starts on the rank page.
        idlePage = IdlePage::Rank;
        idleLeaguePage = 0;
        idlePageShownMs = millis();
    }
    lv_obj_t *target = modeToScreen(mode);
    if (!target) {
        return;
    }
    lv_screen_load_anim(target, anim, 200, 0, false);
    currentMode = mode;
    // Force one-time rebuild of overlay lists after screen switch.
    if (mode == UiMode::EventsList) {
        renderedEventsVersion = 0;
    } else if (mode == UiMode::Squad) {
        renderedSquadVersion = 0;
    }
}

static lv_obj_t *createLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t colorHex, lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(colorHex), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    return label;
}

static void styleScreen(lv_obj_t *screen, uint32_t bgHex) {
    lv_obj_set_style_bg_color(screen, lv_color_hex(bgHex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(screen, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(screen, true, LV_PART_MAIN);
}

static void stylePurpleButton(lv_obj_t *btn) {
    if (!btn) {
        return;
    }
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorButtonPurple), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_PART_MAIN);
}

static void appendAsciiChar(char *out, size_t outLen, size_t &j, char ch) {
    if (!out || outLen == 0 || j + 1 >= outLen) {
        return;
    }
    out[j++] = ch;
    out[j] = '\0';
}

static void sanitizeUtf8ToAscii(const char *in, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < outLen; ++i) {
        const uint8_t c = static_cast<uint8_t>(in[i]);
        if (c < 0x80) {
            appendAsciiChar(out, outLen, j, static_cast<char>(c));
            continue;
        }

        if (c == 0xC3) {
            const uint8_t d = static_cast<uint8_t>(in[i + 1]);
            if (d == 0) {
                break;
            }
            i++;
            switch (d) {
                case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: appendAsciiChar(out, outLen, j, 'A'); break;
                case 0x87: appendAsciiChar(out, outLen, j, 'C'); break;
                case 0x88: case 0x89: case 0x8A: case 0x8B: appendAsciiChar(out, outLen, j, 'E'); break;
                case 0x8C: case 0x8D: case 0x8E: case 0x8F: appendAsciiChar(out, outLen, j, 'I'); break;
                case 0x91: appendAsciiChar(out, outLen, j, 'N'); break;
                case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: appendAsciiChar(out, outLen, j, 'O'); break;
                case 0x99: case 0x9A: case 0x9B: case 0x9C: appendAsciiChar(out, outLen, j, 'U'); break;
                case 0x9D: appendAsciiChar(out, outLen, j, 'Y'); break;
                case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: appendAsciiChar(out, outLen, j, 'a'); break;
                case 0xA7: appendAsciiChar(out, outLen, j, 'c'); break;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: appendAsciiChar(out, outLen, j, 'e'); break;
                case 0xAC: case 0xAD: case 0xAE: case 0xAF: appendAsciiChar(out, outLen, j, 'i'); break;
                case 0xB1: appendAsciiChar(out, outLen, j, 'n'); break;
                case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: appendAsciiChar(out, outLen, j, 'o'); break;
                case 0xB9: case 0xBA: case 0xBB: case 0xBC: appendAsciiChar(out, outLen, j, 'u'); break;
                case 0xBD: case 0xBF: appendAsciiChar(out, outLen, j, 'y'); break;
                case 0x9F:
                    appendAsciiChar(out, outLen, j, 's');
                    appendAsciiChar(out, outLen, j, 's');
                    break;
                default: break;
            }
            continue;
        }

        if (c == 0xC5) {
            const uint8_t d = static_cast<uint8_t>(in[i + 1]);
            if (d == 0) {
                break;
            }
            i++;
            switch (d) {
                case 0x81: appendAsciiChar(out, outLen, j, 'L'); break; // Ł
                case 0x82: appendAsciiChar(out, outLen, j, 'l'); break; // ł
                case 0x9A: case 0x9B: appendAsciiChar(out, outLen, j, 's'); break; // Ś/ś
                case 0xBB: case 0xBC: appendAsciiChar(out, outLen, j, 'z'); break; // Ż/ż
                case 0xB9: case 0xBA: appendAsciiChar(out, outLen, j, 'z'); break; // Ź/ź
                default: break;
            }
        }
    }
}

static bool loadKitImage(const char *team, const char *type) {
    if (!team || !team[0] || !type || !type[0]) {
        Serial.printf("[KIT] invalid args team='%s' type='%s'\n", team ? team : "(null)", type ? type : "(null)");
        return false;
    }
    char path[96];
    snprintf(path, sizeof(path), "/kits/%s_%s_%dx%d.rgb565", team, type, kKitWidth, kKitHeight);

    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[KIT] missing file: %s\n", path);
        return false;
    }
    const size_t bytes = f.read(kitImageBuffer, kKitRgb565Bytes);
    f.close();
    if (bytes != kKitRgb565Bytes) {
        Serial.printf("[KIT] short read: %s (%u/%u)\n", path, static_cast<unsigned>(bytes),
                      static_cast<unsigned>(kKitRgb565Bytes));
        return false;
    }
    Serial.printf("[KIT] loaded: %s\n", path);
    return true;
}

static bool resolveAndLoadKitImage(const UiEventItem &event) {
    if (!event.team[0]) {
        Serial.printf("[KIT] event missing team slug: player='%s' label='%s'\n", event.player, event.label);
        return false;
    }
    const char *type = event.isGk ? "gk" : "outfield";
    if (loadKitImage(event.team, type)) {
        return true;
    }
    if (event.isGk && loadKitImage(event.team, "goalkeeper")) {
        return true;
    }
    if (event.isGk && loadKitImage(event.team, "outfield")) {
        return true;
    }
    if (!event.isGk && loadKitImage(event.team, "player")) {
        return true;
    }
    return false;
}

static void refreshEventsList(const UiRuntimeState &runtime) {
    if (!ui.eventsList) {
        return;
    }
    lv_obj_clean(ui.eventsList);
    if (runtime.recentEventCount == 0) {
        lv_obj_t *empty = createLabel(ui.eventsList, kFontCaption, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(empty, "No events yet");
        lv_obj_center(empty);
        return;
    }

    for (int i = static_cast<int>(runtime.recentEventCount) - 1; i >= 0; --i) {
        const UiEventItem &e = runtime.recentEvents[i];
        lv_obj_t *row = lv_obj_create(ui.eventsList);
        lv_obj_set_size(row, lv_pct(100), 36);
        lv_obj_set_style_min_height(row, 36, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(row, lv_color_hex(kColorBgSurface), LV_PART_MAIN);

        lv_obj_t *left = createLabel(row, kFontBody, kColorTextPrimary, LV_TEXT_ALIGN_LEFT);
        char leftBuf[48];
        snprintf(leftBuf, sizeof(leftBuf), "%s %s", e.icon, e.player);
        lv_label_set_text(left, leftBuf);
        lv_obj_align(left, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t *right = createLabel(row, kFontBody, e.delta >= 0 ? kColorAccentGreen : kColorAccentRed,
                                      LV_TEXT_ALIGN_RIGHT);
        char rightBuf[16];
        snprintf(rightBuf, sizeof(rightBuf), "%+d", e.delta);
        lv_label_set_text(right, rightBuf);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, -6, 0);
    }
}

static void refreshSquadList(const UiRuntimeState &runtime) {
    if (!ui.squadList) {
        return;
    }
    lv_obj_clean(ui.squadList);
    for (size_t i = 0; i < runtime.squadCount; ++i) {
        const UiSquadRow &rowData = runtime.squadRows[i];
        lv_obj_t *row = lv_obj_create(ui.squadList);
        lv_obj_set_size(row, lv_pct(100), 34);
        lv_obj_set_style_min_height(row, 34, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);

        lv_obj_t *name = createLabel(row, &lv_font_montserrat_18, rowData.isBench ? kColorTextSecondary : kColorTextPrimary,
                                     LV_TEXT_ALIGN_LEFT);
        char nameBuf[48];
        snprintf(nameBuf, sizeof(nameBuf), "%s%s%s", rowData.player, rowData.isCaptain ? " C" : "",
                 rowData.isViceCaptain ? " V" : "");
        lv_label_set_text(name, nameBuf);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t *breakdown = createLabel(row, &lv_font_montserrat_16, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(breakdown, rowData.breakdown);
        lv_obj_align(breakdown, LV_ALIGN_CENTER, 28, 0);

        lv_obj_t *points = createLabel(row, kFontBody, kColorTextPrimary, LV_TEXT_ALIGN_RIGHT);
        char ptsBuf[12];
        if (rowData.hasPlayed) {
            snprintf(ptsBuf, sizeof(ptsBuf), "%d", rowData.points);
        } else {
            strlcpy(ptsBuf, "-", sizeof(ptsBuf));
        }
        lv_label_set_text(points, ptsBuf);
        lv_obj_align(points, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    lv_obj_t *footer = lv_obj_create(ui.squadList);
    lv_obj_set_size(footer, lv_pct(100), 42);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(footer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(footer, 0, LV_PART_MAIN);

    static constexpr int kFooterBtnW = 100;
    static constexpr int kFooterBtnH = 32;
    static constexpr int kFooterGap = 6;

    lv_obj_t *demoBtn1 = lv_button_create(footer);
    lv_obj_set_size(demoBtn1, kFooterBtnW, kFooterBtnH);
    lv_obj_align(demoBtn1, LV_ALIGN_LEFT_MID, 6, 0);
    stylePurpleButton(demoBtn1);
    lv_obj_set_style_border_width(demoBtn1, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(demoBtn1, squadDemoNotificationBtnCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *demoBtn1Label = createLabel(demoBtn1, &lv_font_montserrat_14, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(demoBtn1Label, "Notify");
    lv_obj_center(demoBtn1Label);

    lv_obj_t *demoBtn2 = lv_button_create(footer);
    lv_obj_set_size(demoBtn2, kFooterBtnW, kFooterBtnH);
    lv_obj_align_to(demoBtn2, demoBtn1, LV_ALIGN_OUT_RIGHT_MID, kFooterGap, 0);
    stylePurpleButton(demoBtn2);
    lv_obj_set_style_border_width(demoBtn2, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(demoBtn2, squadDemoDeadline45BtnCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *demoBtn2Label = createLabel(demoBtn2, &lv_font_montserrat_14, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(demoBtn2Label, "45m");
    lv_obj_center(demoBtn2Label);

    lv_obj_t *demoBtn3 = lv_button_create(footer);
    lv_obj_set_size(demoBtn3, kFooterBtnW, kFooterBtnH);
    lv_obj_align_to(demoBtn3, demoBtn2, LV_ALIGN_OUT_RIGHT_MID, kFooterGap, 0);
    stylePurpleButton(demoBtn3);
    lv_obj_set_style_border_width(demoBtn3, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(demoBtn3, squadDemoDeadline7BtnCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *demoBtn3Label = createLabel(demoBtn3, &lv_font_montserrat_14, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(demoBtn3Label, "7m");
    lv_obj_center(demoBtn3Label);
}

// ---------------------------------------------------------------------------
// Between-gameweeks carousel
// Rank -> rank graph -> squad price changes -> watchlist -> mini leagues, every screenIntervalSec or on tap.
// Pages with nothing to show are skipped (the rank page is always shown).
// ---------------------------------------------------------------------------
static constexpr size_t kLeaguesPerPage = 5;
static constexpr size_t kPriceRowsMax = 6;
static constexpr int kIdleRowW = 300;

static IdleScreensData *idleUiData = nullptr;      // PSRAM; uiTask's copy
static IdleScreensData *serialIdleData = nullptr;  // PSRAM; serial commands' copy
static DisplaySettings idleUiSettings;
static uint32_t idleUiDataVersion = 0;
static uint32_t idleUiSettingsVersion = 0;

// 950 -> "950", 56,800 -> "57k", 1,234,567 -> "1.2M".
static void formatRankCompact(int rank, char *out, size_t outLen) {
    if (rank < 10000) {
        formatNumberWithCommas(rank, out, outLen);
    } else if (rank < 1000000) {
        snprintf(out, outLen, "%dk", (rank + 500) / 1000);
    } else {
        snprintf(out, outLen, "%.1fM", rank / 1000000.0);
    }
}

static void formatPrice(int tenths, char *out, size_t outLen) {
    snprintf(out, outLen, "%d.%dm", tenths / 10, tenths % 10);
}

// +1 rise, -1 fall, 0 no change expected at the next price update.
static int expectedPriceDirection(const PlayerPrice &p) {
    const int threshold = FPL_PRICE_CHANGE_THRESHOLD_PCT * 10;
    if (p.locked) {
        return 0;
    }
    if (p.projectedPct >= threshold) {
        return 1;
    }
    if (p.projectedPct <= -threshold) {
        return -1;
    }
    return 0;
}

static size_t visibleLeagues(const MiniLeague **out, size_t capacity) {
    size_t n = 0;
    if (!idleUiData->hasLeagues) {
        return 0;
    }
    for (size_t i = 0; i < idleUiData->leagueCount && n < capacity; ++i) {
        if (!displaySettingsIsLeagueHidden(idleUiSettings, idleUiData->leagues[i].id)) {
            out[n++] = &idleUiData->leagues[i];
        }
    }
    return n;
}

static size_t leaguePageCount() {
    const MiniLeague *leagues[kMaxMiniLeagues];
    const size_t n = visibleLeagues(leagues, kMaxMiniLeagues);
    return (n + kLeaguesPerPage - 1) / kLeaguesPerPage;
}

// Index range of the history entries shown on the graph (last N gameweeks, or all).
static size_t rankGraphFirstIndex() {
    const size_t count = idleUiData->history.count;
    const size_t wanted = idleUiSettings.rankGraphGameweeks;
    return (wanted == 0 || wanted >= count) ? 0 : count - wanted;
}

static bool idlePageAvailable(IdlePage page) {
    switch (page) {
        case IdlePage::Rank:
            return true;
        case IdlePage::RankGraph:
            return idleUiData->hasHistory && idleUiData->history.count - rankGraphFirstIndex() >= 2;
        case IdlePage::Prices:
            return idleUiData->hasPrices;
        case IdlePage::Watchlist:
            return idleUiSettings.watchCount > 0 && idleUiData->hasPrices && idleUiData->prices.watchCount > 0;
        case IdlePage::Leagues:
            return leaguePageCount() > 0;
    }
    return false;
}

static void idleTapEventCb(lv_event_t *);

static lv_obj_t *createIdleCarouselScreen(lv_obj_t *&bodyOut) {
    lv_obj_t *screen = lv_obj_create(nullptr);
    styleScreen(screen, kColorBgDeep);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, idleTapEventCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, kDisplayWidth, kDisplayHeight);
    lv_obj_set_pos(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    bodyOut = body;
    return screen;
}

static void createIdleTitle(lv_obj_t *body, const char *title, const char *subtitle) {
    lv_obj_t *titleLabel = createLabel(body, kFontCaption, kColorAccentCyan, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(titleLabel, title);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 46);
    if (subtitle && subtitle[0]) {
        lv_obj_t *subtitleLabel = createLabel(body, kFontMicro, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(subtitleLabel, subtitle);
        lv_obj_align(subtitleLabel, LV_ALIGN_TOP_MID, 0, 72);
    }
}

// A transparent row centred horizontally at vertical offset `yOffset` from the screen centre.
static lv_obj_t *createIdleRow(lv_obj_t *body, int yOffset, int height) {
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, kIdleRowW, height);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, yOffset);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(kColorBgSurface), LV_PART_MAIN);
    return row;
}

static lv_obj_t *createRowLabel(lv_obj_t *row, const lv_font_t *font, uint32_t colorHex, lv_align_t align, int xOfs,
                                int width, const char *text) {
    const lv_text_align_t textAlign = align == LV_ALIGN_RIGHT_MID ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
    lv_obj_t *label = createLabel(row, font, colorHex, textAlign);
    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_label_set_text(label, text);
    lv_obj_align(label, align, xOfs, 0);
    return label;
}

static void createIdleMessage(lv_obj_t *body, const char *text) {
    lv_obj_t *label = createLabel(body, kFontBody, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(label, 280);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

// Row centres for `rows` rows of `rowH`, laid out around a point slightly below the screen centre.
static int idleRowY(size_t index, size_t rows, int rowH) {
    const int blockH = static_cast<int>(rows) * rowH;
    return 10 - blockH / 2 + rowH / 2 + static_cast<int>(index) * rowH;
}

static void formatNextPriceUpdate(char *out, size_t outLen) {
    out[0] = '\0';
    if (!idleUiData->prices.hasNextUpdate || time(nullptr) < kMinValidEpoch) {
        return;
    }
    const time_t t = idleUiData->prices.nextUpdateUtc;
    struct tm local;
    localtime_r(&t, &local);
    snprintf(out, outLen, "next update %02d:%02d", local.tm_hour, local.tm_min);
}

static void renderRankGraphPage() {
    lv_obj_t *body = ui.idleGraphBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    const RankHistory &h = idleUiData->history;
    const size_t first = rankGraphFirstIndex();
    const size_t n = h.count > first ? h.count - first : 0;

    char subtitle[32];
    if (idleUiSettings.rankGraphGameweeks == 0) {
        strlcpy(subtitle, "all gameweeks", sizeof(subtitle));
    } else {
        snprintf(subtitle, sizeof(subtitle), "last %u gameweeks", static_cast<unsigned>(n));
    }
    createIdleTitle(body, "RANK HISTORY", subtitle);
    if (n < 2) {
        createIdleMessage(body, "Not enough gameweeks yet");
        return;
    }

    // Y axis: best (lowest) rank at the top, 10% margin on both ends.
    int best = h.overallRank[first];
    int worst = h.overallRank[first];
    for (size_t i = first; i < h.count; ++i) {
        best = h.overallRank[i] < best ? h.overallRank[i] : best;
        worst = h.overallRank[i] > worst ? h.overallRank[i] : worst;
    }
    int axisTop = static_cast<int>(best * 0.9);
    if (axisTop < 1) {
        axisTop = 1;
    }
    const int axisBottom = static_cast<int>(static_cast<int64_t>(worst) * 11 / 10 + 1);
    const int64_t span = axisBottom - axisTop;

    static constexpr int kChartX = 100;
    static constexpr int kChartY = 118;
    static constexpr int kChartW = 256;
    static constexpr int kChartH = 160;
    static constexpr int kChartPad = 6;  // room for the end points
    // lv_chart maps values in int32 (value * height), so plot on a 0..10000 scale instead of raw ranks.
    static constexpr int32_t kScale = 10000;

    lv_obj_t *chart = lv_chart_create(body);
    lv_obj_set_size(chart, kChartW, kChartH);
    lv_obj_set_pos(chart, kChartX, kChartY);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, kChartPad, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(kColorBgSurface), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 8, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 8, LV_PART_INDICATOR);
    lv_obj_set_style_radius(chart, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_chart_set_point_count(chart, static_cast<uint32_t>(n));
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, kScale);
    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(kColorAccentCyan), LV_CHART_AXIS_PRIMARY_Y);
    for (size_t i = 0; i < n; ++i) {
        const int64_t fromBottom = axisBottom - h.overallRank[first + i];
        lv_chart_set_value_by_id(chart, series, static_cast<uint32_t>(i), static_cast<int32_t>(fromBottom * kScale / span));
    }
    lv_chart_refresh(chart);

    // Y labels at the three division lines.
    const int contentH = kChartH - 2 * kChartPad;
    const int axisValues[3] = {axisTop, axisTop + static_cast<int>(span / 2), axisBottom};
    char buf[24];
    for (int i = 0; i < 3; ++i) {
        formatRankCompact(axisValues[i], buf, sizeof(buf));
        lv_obj_t *label = createLabel(body, kFontMicro, kColorTextSecondary, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(label, 56);
        lv_label_set_text(label, buf);
        lv_obj_set_pos(label, kChartX - 60, kChartY + kChartPad + (contentH * i) / 2 - 8);
    }

    // X labels: every GW when they fit, otherwise first, middle and last.
    const int contentW = kChartW - 2 * kChartPad;
    const bool labelAll = n <= 10;
    for (size_t i = 0; i < n; ++i) {
        if (!labelAll && i != 0 && i != n - 1 && i != (n - 1) / 2) {
            continue;
        }
        snprintf(buf, sizeof(buf), n <= 5 || !labelAll ? "GW%d" : "%d", h.gw[first + i]);
        lv_obj_t *label = createLabel(body, kFontMicro, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(label, 44);
        lv_label_set_text(label, buf);
        const int x = kChartX + kChartPad + static_cast<int>((contentW * static_cast<int>(i)) / static_cast<int>(n - 1));
        lv_obj_set_pos(label, x - 22, kChartY + kChartH + 4);
    }

    // Latest rank with the change from the previous gameweek on the graph.
    const int latest = h.overallRank[h.count - 1];
    const int previous = h.overallRank[h.count - 2];
    char rankBuf[24];
    formatNumberWithCommas(latest, rankBuf, sizeof(rankBuf));
    snprintf(buf, sizeof(buf), "GW%d  %s", h.gw[h.count - 1], rankBuf);
    lv_obj_t *footer = createLabel(body, kFontBody, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(footer, buf);
    lv_obj_align(footer, LV_ALIGN_CENTER, -10, 118);
    lv_obj_t *arrow = createLabel(body, kFontBody,
                                  latest < previous ? kColorAccentGreen :
                                  (latest > previous ? kColorAccentRed : kColorTextSecondary),
                                  LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(arrow, latest < previous ? LV_SYMBOL_UP : (latest > previous ? LV_SYMBOL_DOWN : "-"));
    lv_obj_align_to(arrow, footer, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

static void renderPricesPage() {
    lv_obj_t *body = ui.idlePricesBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    char subtitle[40];
    char nextUpdate[24];
    formatNextPriceUpdate(nextUpdate, sizeof(nextUpdate));
    snprintf(subtitle, sizeof(subtitle), "my squad%s%s", nextUpdate[0] ? " - " : "", nextUpdate);
    createIdleTitle(body, "PRICE CHANGES", subtitle);

    const PriceData &prices = idleUiData->prices;
    const PlayerPrice *changes[16];
    size_t changeCount = 0;
    for (size_t i = 0; i < prices.squadCount; ++i) {
        if (expectedPriceDirection(prices.squad[i]) != 0) {
            changes[changeCount++] = &prices.squad[i];
        }
    }
    if (changeCount == 0) {
        createIdleMessage(body, "No expected\nprice changes");
        return;
    }
    // Rises first, then falls; strongest projection first within each.
    for (size_t i = 1; i < changeCount; ++i) {
        for (size_t j = i; j > 0; --j) {
            const PlayerPrice *a = changes[j - 1];
            const PlayerPrice *b = changes[j];
            if (b->projectedPct > a->projectedPct) {
                changes[j - 1] = b;
                changes[j] = a;
            }
        }
    }

    static constexpr int kRowH = 38;
    const size_t shown = changeCount > kPriceRowsMax ? kPriceRowsMax - 1 : changeCount;
    const size_t rows = changeCount > kPriceRowsMax ? kPriceRowsMax : changeCount;
    char buf[32];
    for (size_t i = 0; i < shown; ++i) {
        const PlayerPrice &p = *changes[i];
        const int dir = expectedPriceDirection(p);
        lv_obj_t *row = createIdleRow(body, idleRowY(i, rows, kRowH), kRowH);
        createRowLabel(row, kFontBody, kColorTextPrimary, LV_ALIGN_LEFT_MID, 8, 170, p.name);
        char priceBuf[12];
        formatPrice(p.nowCost, priceBuf, sizeof(priceBuf));
        snprintf(buf, sizeof(buf), "%s %s", priceBuf, dir > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
        createRowLabel(row, kFontBody, dir > 0 ? kColorAccentGreen : kColorAccentRed, LV_ALIGN_RIGHT_MID, -8, 0, buf);
    }
    if (shown < changeCount) {
        lv_obj_t *row = createIdleRow(body, idleRowY(shown, rows, kRowH), kRowH);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        snprintf(buf, sizeof(buf), "+%u more", static_cast<unsigned>(changeCount - shown));
        lv_obj_t *more = createLabel(row, kFontCaption, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
        lv_label_set_text(more, buf);
        lv_obj_center(more);
    }
}

static void renderWatchlistPage() {
    lv_obj_t *body = ui.idleWatchBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    char nextUpdate[24];
    formatNextPriceUpdate(nextUpdate, sizeof(nextUpdate));
    createIdleTitle(body, "WATCHLIST", nextUpdate);

    const PriceData &prices = idleUiData->prices;
    const size_t rows = prices.watchCount;
    const int rowH = rows > 6 ? 28 : 38;
    const lv_font_t *nameFont = rows > 6 ? &lv_font_montserrat_16 : kFontCaption;
    char buf[32];
    for (size_t i = 0; i < rows; ++i) {
        const PlayerPrice &p = prices.watch[i];
        const int dir = expectedPriceDirection(p);
        lv_obj_t *row = createIdleRow(body, idleRowY(i, rows, rowH), rowH);
        createRowLabel(row, nameFont, kColorTextPrimary, LV_ALIGN_LEFT_MID, 8, 118, p.name);

        // Price and progress toward the next change, e.g. "9.5m +50%".
        char priceBuf[12];
        formatPrice(p.nowCost, priceBuf, sizeof(priceBuf));
        snprintf(buf, sizeof(buf), "%s %+d%%", priceBuf, p.projectedPct / 10);
        createRowLabel(row, kFontMicro, kColorTextSecondary, LV_ALIGN_LEFT_MID, 132, 0, buf);

        uint32_t color = kColorTextSecondary;
        const char *status = "No change";
        if (p.locked) {
            status = "Locked";
        } else if (dir > 0) {
            status = LV_SYMBOL_UP " Rise";
            color = kColorAccentGreen;
        } else if (dir < 0) {
            status = LV_SYMBOL_DOWN " Fall";
            color = kColorAccentRed;
        }
        createRowLabel(row, &lv_font_montserrat_16, color, LV_ALIGN_RIGHT_MID, -8, 0, status);
    }
}

static void renderLeaguesPage() {
    lv_obj_t *body = ui.idleLeaguesBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    const MiniLeague *leagues[kMaxMiniLeagues];
    const size_t total = visibleLeagues(leagues, kMaxMiniLeagues);
    const size_t pages = (total + kLeaguesPerPage - 1) / kLeaguesPerPage;
    if (idleLeaguePage >= pages) {
        idleLeaguePage = 0;
    }

    char subtitle[24] = "";
    if (pages > 1) {
        snprintf(subtitle, sizeof(subtitle), "%u / %u", static_cast<unsigned>(idleLeaguePage + 1),
                 static_cast<unsigned>(pages));
    }
    createIdleTitle(body, "MINI LEAGUES", subtitle);
    if (total == 0) {
        createIdleMessage(body, "No mini leagues");
        return;
    }

    static constexpr int kRowH = 42;
    const size_t first = idleLeaguePage * kLeaguesPerPage;
    const size_t rows = (total - first) < kLeaguesPerPage ? (total - first) : kLeaguesPerPage;
    char buf[24];
    for (size_t i = 0; i < rows; ++i) {
        const MiniLeague &lg = *leagues[first + i];
        lv_obj_t *row = createIdleRow(body, idleRowY(i, kLeaguesPerPage, kRowH), kRowH);
        createRowLabel(row, kFontCaption, kColorTextPrimary, LV_ALIGN_LEFT_MID, 8, 160, lg.name);

        char rankBuf[16];
        formatNumberWithCommas(lg.rank, rankBuf, sizeof(rankBuf));
        snprintf(buf, sizeof(buf), "#%s", rankBuf);
        createRowLabel(row, kFontCaption, kColorTextPrimary, LV_ALIGN_RIGHT_MID, -70, 0, buf);

        // Movement since the end of the previous gameweek (positive = climbed).
        const int moved = lg.lastRank > 0 ? lg.lastRank - lg.rank : 0;
        uint32_t color = kColorTextSecondary;
        if (moved > 0) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_UP "%d", moved);
            color = kColorAccentGreen;
        } else if (moved < 0) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN "%d", -moved);
            color = kColorAccentRed;
        } else {
            strlcpy(buf, "-", sizeof(buf));
        }
        lv_obj_t *move = createRowLabel(row, &lv_font_montserrat_16, color, LV_ALIGN_RIGHT_MID, -8, 60, buf);
        lv_obj_set_style_text_align(move, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    }
}

static void renderIdlePage(IdlePage page) {
    switch (page) {
        case IdlePage::Rank: break;  // updated continuously by updateModeUi
        case IdlePage::RankGraph: renderRankGraphPage(); break;
        case IdlePage::Prices: renderPricesPage(); break;
        case IdlePage::Watchlist: renderWatchlistPage(); break;
        case IdlePage::Leagues: renderLeaguesPage(); break;
    }
}

static void showIdlePage(IdlePage page) {
    idlePage = page;
    idlePageShownMs = millis();
    renderIdlePage(page);
    lv_obj_t *screen = idlePageScreen(page);
    if (currentMode == UiMode::Idle && screen && lv_screen_active() != screen) {
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}

static void advanceIdlePage() {
    if (idlePage == IdlePage::Leagues && idleLeaguePage + 1 < leaguePageCount()) {
        ++idleLeaguePage;
        idlePageShownMs = millis();
        renderLeaguesPage();
        return;
    }
    IdlePage next = idlePage;
    for (uint8_t i = 0; i < kIdlePageCount; ++i) {
        next = static_cast<IdlePage>((static_cast<uint8_t>(next) + 1) % kIdlePageCount);
        if (idlePageAvailable(next)) {
            break;
        }
    }
    idleLeaguePage = 0;
    showIdlePage(next);
}

static void idleTapEventCb(lv_event_t *) {
    if (currentMode == UiMode::Idle) {
        advanceIdlePage();
    }
}

// Called from uiTask every loop: picks up new data/settings and advances the carousel on its timer.
static void serviceIdleCarousel() {
    bool changed = false;
    if (copyIdleScreensDataIfChanged(*idleUiData, idleUiDataVersion)) {
        idleUiDataVersion = idleUiData->version;
        changed = true;
    }
    const uint32_t settingsVersion = displaySettingsVersion();
    if (settingsVersion != 0 && settingsVersion != idleUiSettingsVersion) {
        displaySettingsGet(idleUiSettings);
        idleUiSettingsVersion = settingsVersion;
        changed = true;
    }

    if (currentMode != UiMode::Idle) {
        return;
    }
    if (!idlePageAvailable(idlePage)) {
        advanceIdlePage();
        return;
    }
    if (changed) {
        renderIdlePage(idlePage);
    }
    const uint32_t intervalMs = static_cast<uint32_t>(idleUiSettings.screenIntervalSec) * 1000U;
    if (intervalMs > 0 && millis() - idlePageShownMs >= intervalMs) {
        advanceIdlePage();
    }
}

static void backFromEventsCb(lv_event_t *) {
    loadMode(UiMode::Live, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void backFromSquadCb(lv_event_t *) {
    loadMode(UiMode::Live, LV_SCR_LOAD_ANIM_MOVE_TOP);
}

static void showEventsEventCb(lv_event_t *) {
    loadMode(UiMode::EventsList, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void showSquadFromHold(void) {
    loadMode(UiMode::Squad, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
    holdTriggered = true;
    if (ui.liveHoldArc) {
        lv_obj_add_flag(ui.liveHoldArc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_value(ui.liveHoldArc, 0);
    }
}

static void livePressEventCb(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (ui.liveTickerBtn) {
        lv_indev_t *indev = lv_indev_active();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            lv_area_t tickerArea;
            lv_obj_get_coords(ui.liveTickerBtn, &tickerArea);
            const bool inTicker = (p.x >= tickerArea.x1 && p.x <= tickerArea.x2 && p.y >= tickerArea.y1 && p.y <= tickerArea.y2);
            if (inTicker) {
                return;
            }
        }
    }

    if (code == LV_EVENT_PRESSED) {
        holdStartMs = millis();
        holdTriggered = false;
        if (ui.liveHoldArc) {
            lv_obj_remove_flag(ui.liveHoldArc, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_value(ui.liveHoldArc, 0);
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (holdStartMs == 0 || holdTriggered || !ui.liveHoldArc) {
            return;
        }
        const uint32_t elapsed = millis() - holdStartMs;
        int progress = static_cast<int>((elapsed * 100U) / 3000U);
        if (progress > 100) {
            progress = 100;
        }
        lv_arc_set_value(ui.liveHoldArc, progress);
        if (elapsed >= 3000U) {
            showSquadFromHold();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        holdStartMs = 0;
        if (!holdTriggered && ui.liveHoldArc) {
            lv_obj_add_flag(ui.liveHoldArc, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_value(ui.liveHoldArc, 0);
        }
        holdTriggered = false;
    }
}

static lv_obj_t *createOverlayListScreen(lv_obj_t *screen, const char *title, lv_event_cb_t backCb) {
    static constexpr int kPanelX = 340;
    static constexpr int kPanelY = 340;
    static constexpr int kPanelTop = 22;
    static constexpr int kListW = 324;
    static constexpr int kListH = 286;
    static constexpr int kChordY = (kPanelTop + kPanelY) - 1;

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, kPanelX, kPanelY);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, kPanelTop);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(kColorBgSurface), LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);

    lv_obj_t *titleLabel = createLabel(panel, kFontCaption, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(titleLabel, title);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_set_size(list, kListW, kListH);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 2, LV_PART_MAIN);

    lv_obj_t *chordLine = lv_obj_create(screen);
    lv_obj_set_size(chordLine, kPanelX, 2);
    lv_obj_align(chordLine, LV_ALIGN_TOP_MID, 0, kChordY);
    lv_obj_set_style_bg_color(chordLine, lv_color_hex(kColorButtonPurple), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chordLine, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(chordLine, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(chordLine, 0, LV_PART_MAIN);

    lv_obj_t *back = lv_button_create(screen);
    lv_obj_set_size(back, kPanelX, kDisplayHeight - kChordY);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, kChordY);
    lv_obj_set_style_radius(back, 0, LV_PART_MAIN);
    stylePurpleButton(back);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, backCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLabel = createLabel(back, &lv_font_montserrat_32, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(backLabel, "<");
    lv_obj_center(backLabel);

    lv_obj_move_foreground(chordLine);
    lv_obj_move_foreground(back);
    return list;
}

static void createUi(void) {
    memset(&ui, 0, sizeof(ui));
    ui.screenIdle = lv_obj_create(nullptr);
    ui.screenDeadline = lv_obj_create(nullptr);
    ui.screenFinalHour = lv_obj_create(nullptr);
    ui.screenLive = lv_obj_create(nullptr);
    ui.screenPopup = lv_obj_create(nullptr);
    ui.screenEvents = lv_obj_create(nullptr);
    ui.screenSquad = lv_obj_create(nullptr);

    styleScreen(ui.screenIdle, kColorBgDeep);
    styleScreen(ui.screenDeadline, kColorBgDeep);
    styleScreen(ui.screenFinalHour, 0x0C0B14);
    styleScreen(ui.screenLive, kColorBgDeep);
    styleScreen(ui.screenPopup, kColorBgDeep);
    styleScreen(ui.screenEvents, kColorBgDeep);
    styleScreen(ui.screenSquad, kColorBgDeep);

    lv_obj_t *idleRing = lv_arc_create(ui.screenIdle);
    lv_obj_set_size(idleRing, 340, 340);
    lv_obj_center(idleRing);
    lv_obj_set_style_arc_color(idleRing, lv_color_hex(kColorBgSurface), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(idleRing, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_width(idleRing, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_width(idleRing, 0, LV_PART_INDICATOR);
    lv_obj_remove_style(idleRing, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(idleRing, LV_OBJ_FLAG_CLICKABLE);

    ui.idleRankArrow = createLabel(ui.screenIdle, kFontBody, kColorAccentGreen, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.idleRankArrow, LV_ALIGN_CENTER, -68, -48);
    ui.idleRankValue = createLabel(ui.screenIdle, kFontHero, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.idleRankValue, LV_ALIGN_CENTER, 16, -48);
    ui.idleGwPoints = createLabel(ui.screenIdle, &lv_font_montserrat_26, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.idleGwPoints, LV_ALIGN_CENTER, 0, 36);
    ui.idleTotalPoints = createLabel(ui.screenIdle, kFontCaption, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.idleTotalPoints, LV_ALIGN_CENTER, 0, 78);
    ui.statusLabel = createLabel(ui.screenIdle, kFontMicro, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.statusLabel, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_event_cb(ui.screenIdle, idleTapEventCb, LV_EVENT_CLICKED, nullptr);

    ui.screenIdleGraph = createIdleCarouselScreen(ui.idleGraphBody);
    ui.screenIdlePrices = createIdleCarouselScreen(ui.idlePricesBody);
    ui.screenIdleWatch = createIdleCarouselScreen(ui.idleWatchBody);
    ui.screenIdleLeagues = createIdleCarouselScreen(ui.idleLeaguesBody);

    ui.deadlineLabel = createLabel(ui.screenDeadline, &lv_font_montserrat_28, kColorAccentAmber, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(ui.deadlineLabel, "DEADLINE");
    lv_obj_align(ui.deadlineLabel, LV_ALIGN_CENTER, 0, -110);
    ui.deadlineCountdown = createLabel(ui.screenDeadline, kFontHero, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.deadlineCountdown, LV_ALIGN_CENTER, 0, -20);
    ui.deadlineMeta = createLabel(ui.screenDeadline, &lv_font_montserrat_22, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.deadlineMeta, LV_ALIGN_CENTER, 0, 84);

    ui.finalCountdown = createLabel(ui.screenFinalHour, kFontHero, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_center(ui.finalCountdown);

    ui.liveTitle = createLabel(ui.screenLive, &lv_font_montserrat_16, kColorAccentCyan, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.liveTitle, LV_ALIGN_TOP_MID, 0, 24);
    ui.liveDot = lv_obj_create(ui.screenLive);
    lv_obj_set_size(ui.liveDot, 8, 8);
    lv_obj_align(ui.liveDot, LV_ALIGN_TOP_MID, 84, 30);
    lv_obj_set_style_radius(ui.liveDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.liveDot, lv_color_hex(kColorAccentCyan), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.liveDot, 0, LV_PART_MAIN);
    ui.livePoints = createLabel(ui.screenLive, kFontHero, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.livePoints, LV_ALIGN_CENTER, 0, -46);
    ui.liveRank = createLabel(ui.screenLive, &lv_font_montserrat_26, kColorAccentGreen, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.liveRank, LV_ALIGN_CENTER, 0, 26);
    static constexpr int kLiveEventsTopY = static_cast<int>(kDisplayHeight) - 116;
    lv_obj_t *liveEventsLine = lv_obj_create(ui.screenLive);
    lv_obj_set_size(liveEventsLine, kDisplayWidth, 2);
    lv_obj_align(liveEventsLine, LV_ALIGN_TOP_MID, 0, kLiveEventsTopY);
    lv_obj_set_style_bg_color(liveEventsLine, lv_color_hex(kColorButtonPurple), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(liveEventsLine, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(liveEventsLine, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(liveEventsLine, 0, LV_PART_MAIN);

    ui.liveTickerBtn = lv_button_create(ui.screenLive);
    lv_obj_set_size(ui.liveTickerBtn, kDisplayWidth, static_cast<int>(kDisplayHeight) - kLiveEventsTopY);
    lv_obj_align(ui.liveTickerBtn, LV_ALIGN_TOP_MID, 0, kLiveEventsTopY);
    stylePurpleButton(ui.liveTickerBtn);
    lv_obj_set_style_radius(ui.liveTickerBtn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.liveTickerBtn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(ui.liveTickerBtn, showEventsEventCb, LV_EVENT_CLICKED, nullptr);
    ui.liveTickerLabel = createLabel(ui.liveTickerBtn, kFontBody, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.liveTickerLabel, LV_ALIGN_TOP_MID, 0, 10);
    ui.liveHoldArc = lv_arc_create(ui.screenLive);
    lv_obj_set_size(ui.liveHoldArc, 360, 360);
    lv_obj_center(ui.liveHoldArc);
    lv_arc_set_range(ui.liveHoldArc, 0, 100);
    lv_obj_set_style_arc_width(ui.liveHoldArc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.liveHoldArc, lv_color_hex(kColorAccentCyan), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ui.liveHoldArc, 0, LV_PART_MAIN);
    lv_obj_remove_style(ui.liveHoldArc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(ui.liveHoldArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui.liveHoldArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ui.screenLive, livePressEventCb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(ui.screenLive, livePressEventCb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(ui.screenLive, livePressEventCb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(ui.screenLive, livePressEventCb, LV_EVENT_PRESS_LOST, nullptr);

    ui.popupTitle = createLabel(ui.screenPopup, kFontLarge, kColorAccentGreen, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.popupTitle, LV_ALIGN_CENTER, 0, -140);
    memset(&kitImageDsc, 0, sizeof(kitImageDsc));
    kitImageDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    kitImageDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    kitImageDsc.header.w = kKitWidth;
    kitImageDsc.header.h = kKitHeight;
    kitImageDsc.data_size = kKitRgb565Bytes;
    kitImageDsc.data = kitImageBuffer;
    ui.popupKit = lv_image_create(ui.screenPopup);
    lv_image_set_src(ui.popupKit, &kitImageDsc);
    lv_obj_align(ui.popupKit, LV_ALIGN_CENTER, 0, -24);
    ui.popupPlayer = createLabel(ui.screenPopup, kFontLarge, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.popupPlayer, LV_ALIGN_CENTER, 0, 68);
    ui.popupDelta = createLabel(ui.screenPopup, &lv_font_montserrat_28, kColorAccentGreen, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.popupDelta, LV_ALIGN_CENTER, 0, 118);
    ui.popupTotal = createLabel(ui.screenPopup, kFontBody, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.popupTotal, LV_ALIGN_CENTER, 0, 154);

    ui.eventsList = createOverlayListScreen(ui.screenEvents, "EVENTS", backFromEventsCb);
    ui.squadList = createOverlayListScreen(ui.screenSquad, "MY SQUAD", backFromSquadCb);

    lv_screen_load(ui.screenIdle);
    currentMode = UiMode::Idle;
}

static void setSharedStatus(const char *text, uint32_t colorHex) {
    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    strlcpy(sharedUiState.statusText, text, sizeof(sharedUiState.statusText));
    sharedUiState.statusColor = colorHex;
    sharedUiState.version++;
    xSemaphoreGive(sharedUiMutex);
}

static void setSharedGwPoints(int points) {
    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    sharedUiState.gwPoints = points;
    sharedUiState.hasGwPoints = true;
    sharedUiState.version++;
    xSemaphoreGive(sharedUiMutex);
}

static void setSharedGwStateText(const char *text) {
    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    strlcpy(sharedUiState.gwStateText, text, sizeof(sharedUiState.gwStateText));
    sharedUiState.version++;
    xSemaphoreGive(sharedUiMutex);
}

static void setSharedGameweekContext(bool isLiveGw, int currentGw, int nextGw, bool hasNextGw, time_t deadlineUtc,
                                     bool hasDeadline) {
    ledRingSetDeadlineCountdown(hasDeadline && !isLiveGw, deadlineUtc);

    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    sharedUiState.isLiveGw = isLiveGw;
    sharedUiState.currentGw = currentGw;
    sharedUiState.nextGw = nextGw;
    sharedUiState.hasNextGw = hasNextGw;
    sharedUiState.nextDeadlineUtc = deadlineUtc;
    sharedUiState.hasNextDeadline = hasDeadline;
    sharedUiState.version++;
    xSemaphoreGive(sharedUiMutex);
}

static void setSharedRankData(int overallRank, int rankDiff, bool hasRankData) {
    ledRingSetRankTrend(rankDiff, hasRankData);

    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    sharedUiState.overallRank = overallRank;
    sharedUiState.rankDiff = rankDiff;
    sharedUiState.hasRankData = hasRankData;
    sharedUiState.version++;
    xSemaphoreGive(sharedUiMutex);
}

static void setSharedTotalPoints(int totalPoints, bool hasTotalPoints) {
    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    sharedUiState.totalPoints = totalPoints;
    sharedUiState.hasTotalPoints = hasTotalPoints;
    sharedUiState.version++;
    xSemaphoreGive(sharedUiMutex);
}

static void setSharedFreshness(bool isStale, uint32_t lastApiUpdateMs) {
    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    sharedUiState.isStale = isStale;
    sharedUiState.lastApiUpdateMs = lastApiUpdateMs;
    sharedUiState.version++;
    xSemaphoreGive(sharedUiMutex);
}

static bool readSharedUiState(SharedUiState &out) {
    if (!sharedUiMutex) {
        return false;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    out = sharedUiState;
    xSemaphoreGive(sharedUiMutex);
    return true;
}

static bool isScriptedDemoActive() {
    if (!scriptedDemoMutex) {
        return false;
    }
    if (xSemaphoreTake(scriptedDemoMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }
    const bool active = scriptedDemoState.active;
    xSemaphoreGive(scriptedDemoMutex);
    return active;
}

static void fillScriptedEventFromSlot(int slot, const char *label, int delta, const char *icon, int totalBefore,
                                      UiEventItem &eventOut) {
    memset(&eventOut, 0, sizeof(eventOut));
    strlcpy(eventOut.icon, icon, sizeof(eventOut.icon));
    sanitizeUtf8ToAscii(label, eventOut.label, sizeof(eventOut.label));
    snprintf(eventOut.player, sizeof(eventOut.player), "Player %d", slot);
    eventOut.delta = delta;
    eventOut.totalBefore = totalBefore;
    eventOut.totalAfter = totalBefore + delta;
    eventOut.epochMs = millis();

    if (!uiRuntimeMutex) {
        return;
    }
    if (xSemaphoreTake(uiRuntimeMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    const size_t idx = (slot > 0) ? static_cast<size_t>(slot - 1) : 0;
    if (idx < uiRuntimeState.squadCount) {
        const UiSquadRow &row = uiRuntimeState.squadRows[idx];
        sanitizeUtf8ToAscii(row.player, eventOut.player, sizeof(eventOut.player));
        strlcpy(eventOut.team, row.team, sizeof(eventOut.team));
        eventOut.isGk = row.isGk;
    }
    xSemaphoreGive(uiRuntimeMutex);
}

static void startNotificationScriptedDemo() {
    SharedUiState local;
    if (!readSharedUiState(local)) {
        memset(&local, 0, sizeof(local));
    }

    const int baseRank = (local.hasRankData && local.overallRank > 0) ? local.overallRank : 100000;
    const int currentGw = local.currentGw > 0 ? local.currentGw : 1;
    const int nextGw = local.hasNextGw ? local.nextGw : (currentGw + 1);
    const bool hasNextGw = local.hasNextGw || nextGw > 0;

    if (scriptedDemoMutex && xSemaphoreTake(scriptedDemoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        scriptedDemoState.active = true;
        scriptedDemoState.kind = ScriptedDemoKind::Notification;
        scriptedDemoState.stage = 0;
        scriptedDemoState.baseRank = baseRank;
        scriptedDemoState.triggerAtMs = millis() + kDemoNotificationDelayMs;
        xSemaphoreGive(scriptedDemoMutex);
    }

    setSharedGameweekContext(true, currentGw, nextGw, hasNextGw, local.nextDeadlineUtc, local.hasNextDeadline);
    setSharedRankData(baseRank, -500, true);
    setSharedStatus("Demo: rank dropping", 0xFF5A5A);
    loadMode(UiMode::Live, LV_SCR_LOAD_ANIM_FADE_ON);
}

static void startDeadlineScriptedDemo(int32_t secondsToDeadline) {
    if (scriptedDemoMutex && xSemaphoreTake(scriptedDemoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        scriptedDemoState = ScriptedDemoState{};
        xSemaphoreGive(scriptedDemoMutex);
    }

    ensureUkTimeConfigured();
    const time_t nowUtc = time(nullptr);
    if (nowUtc <= 100000) {
        setSharedStatus("Demo: time unavailable", 0xFF5A5A);
        return;
    }

    SharedUiState local;
    if (!readSharedUiState(local)) {
        memset(&local, 0, sizeof(local));
    }

    const int currentGw = local.currentGw > 0 ? local.currentGw : 1;
    const int nextGw = local.hasNextGw ? local.nextGw : (currentGw + 1);
    const bool hasNextGw = local.hasNextGw || nextGw > 0;
    const time_t deadlineUtc = nowUtc + secondsToDeadline;
    setSharedGameweekContext(false, currentGw, nextGw, hasNextGw, deadlineUtc, true);
    setSharedStatus("Demo: custom deadline", 0x00E5FF);
    loadMode(UiMode::FinalHour, LV_SCR_LOAD_ANIM_FADE_ON);
}

static void squadDemoNotificationBtnCb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    startNotificationScriptedDemo();
}

static void squadDemoDeadline45BtnCb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    startDeadlineScriptedDemo(kDemoDeadline45mSec);
}

static void squadDemoDeadline7BtnCb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    startDeadlineScriptedDemo(kDemoDeadline7mSec);
}

static void serviceScriptedDemo() {
    if (!scriptedDemoMutex) {
        return;
    }

    int baseRank = 0;
    bool triggerEvents = false;
    const uint32_t nowMs = millis();
    if (xSemaphoreTake(scriptedDemoMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    if (scriptedDemoState.active && scriptedDemoState.kind == ScriptedDemoKind::Notification &&
        scriptedDemoState.stage == 0 && nowMs >= scriptedDemoState.triggerAtMs) {
        scriptedDemoState.stage = 1;
        baseRank = scriptedDemoState.baseRank;
        scriptedDemoState.active = false;
        scriptedDemoState.kind = ScriptedDemoKind::None;
        triggerEvents = true;
    }
    xSemaphoreGive(scriptedDemoMutex);

    if (!triggerEvents) {
        return;
    }

    UiEventItem goalEvent;
    fillScriptedEventFromSlot(2, "GOAL!", 5, "G", 1, goalEvent);
    pushUiEvent(goalEvent);

    UiEventItem assistEvent;
    fillScriptedEventFromSlot(5, "ASSIST!", 3, "A", 1, assistEvent);
    pushUiEvent(assistEvent);

    SharedUiState local;
    if (readSharedUiState(local)) {
        setSharedGwPoints(local.gwPoints + goalEvent.delta + assistEvent.delta);
        if (local.hasTotalPoints) {
            setSharedTotalPoints(local.totalPoints + goalEvent.delta + assistEvent.delta, true);
        }
    }

    const int improvedRank = (baseRank > 10000) ? (baseRank - 10000) : 1;
    setSharedRankData(improvedRank, 10000, true);
    setSharedStatus("Demo: rank recovered", 0x38D39F);
    loadMode(UiMode::Live, LV_SCR_LOAD_ANIM_FADE_ON);
}

static void snapshotUiRuntime(UiRuntimeState &out) {
    memset(&out, 0, sizeof(out));
    if (!uiRuntimeMutex) {
        return;
    }
    if (xSemaphoreTake(uiRuntimeMutex, pdMS_TO_TICKS(30)) != pdTRUE) {
        return;
    }
    out = uiRuntimeState;
    xSemaphoreGive(uiRuntimeMutex);
}

static void pushUiEvent(const UiEventItem &event) {
    if (!uiRuntimeMutex) {
        return;
    }
    if (xSemaphoreTake(uiRuntimeMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (uiRuntimeState.recentEventCount < kMaxUiEvents) {
        uiRuntimeState.recentEvents[uiRuntimeState.recentEventCount++] = event;
    } else {
        for (size_t i = 1; i < kMaxUiEvents; ++i) {
            uiRuntimeState.recentEvents[i - 1] = uiRuntimeState.recentEvents[i];
        }
        uiRuntimeState.recentEvents[kMaxUiEvents - 1] = event;
    }

    if (uiRuntimeState.popupCount < kMaxPopupEvents) {
        uiRuntimeState.popupQueue[uiRuntimeState.popupTail] = event;
        uiRuntimeState.popupTail = (uiRuntimeState.popupTail + 1) % kMaxPopupEvents;
        uiRuntimeState.popupCount++;
    }
    uiRuntimeState.eventVersion++;
    xSemaphoreGive(uiRuntimeMutex);
}

static bool popUiPopup(UiEventItem &eventOut) {
    if (!uiRuntimeMutex) {
        return false;
    }
    if (xSemaphoreTake(uiRuntimeMutex, pdMS_TO_TICKS(30)) != pdTRUE) {
        return false;
    }
    if (uiRuntimeState.popupCount == 0) {
        xSemaphoreGive(uiRuntimeMutex);
        return false;
    }

    eventOut = uiRuntimeState.popupQueue[uiRuntimeState.popupHead];
    uiRuntimeState.popupHead = (uiRuntimeState.popupHead + 1) % kMaxPopupEvents;
    uiRuntimeState.popupCount--;
    xSemaphoreGive(uiRuntimeMutex);
    return true;
}

static void clearUiEvents() {
    if (!uiRuntimeMutex) {
        return;
    }
    if (xSemaphoreTake(uiRuntimeMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    uiRuntimeState.recentEventCount = 0;
    uiRuntimeState.popupHead = 0;
    uiRuntimeState.popupTail = 0;
    uiRuntimeState.popupCount = 0;
    uiRuntimeState.eventVersion++;
    xSemaphoreGive(uiRuntimeMutex);
}

static void updateSharedSquadFromPicks(const TeamPick *picks, size_t pickCount) {
    if (!uiRuntimeMutex) {
        return;
    }
    if (xSemaphoreTake(uiRuntimeMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    uiRuntimeState.squadCount = 0;
    for (size_t i = 0; i < pickCount && i < kMaxSquadRows; ++i) {
        UiSquadRow &row = uiRuntimeState.squadRows[uiRuntimeState.squadCount++];
        memset(&row, 0, sizeof(row));
        sanitizeUtf8ToAscii(picks[i].playerName.length() ? picks[i].playerName.c_str() : "unknown", row.player,
                            sizeof(row.player));
        strlcpy(row.team, picks[i].teamShortName, sizeof(row.team));
        row.points = picks[i].live.totalPoints * picks[i].multiplier;
        row.hasPlayed = picks[i].live.minutes > 0;
        row.isCaptain = picks[i].isCaptain;
        row.isViceCaptain = picks[i].isViceCaptain;
        row.isBench = picks[i].squadPosition > 11;
        row.isGk = picks[i].elementType == 1;

        if (picks[i].live.goalsScored > 0) {
            snprintf(row.breakdown, sizeof(row.breakdown), "G +%d", goalPointsForElementType(picks[i].elementType));
        } else if (picks[i].live.assists > 0) {
            strlcpy(row.breakdown, "A +3", sizeof(row.breakdown));
        } else if (picks[i].live.cleanSheets > 0) {
            strlcpy(row.breakdown, "CS +4", sizeof(row.breakdown));
        } else if (picks[i].live.saves >= 3) {
            strlcpy(row.breakdown, "SV +1", sizeof(row.breakdown));
        } else if (picks[i].live.yellowCards > 0) {
            strlcpy(row.breakdown, "YC -1", sizeof(row.breakdown));
        } else if (picks[i].live.redCards > 0) {
            strlcpy(row.breakdown, "RC -3", sizeof(row.breakdown));
        } else {
            row.breakdown[0] = '\0';
        }
    }
    uiRuntimeState.squadVersion++;
    xSemaphoreGive(uiRuntimeMutex);
}

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

static bool parseIsoUtcToEpoch(const char *iso, time_t &epochOut) {
    if (!iso) {
        return false;
    }
    int y = 0;
    int mon = 0;
    int day = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    int n = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d%n", &y, &mon, &day, &hh, &mm, &ss, &n) != 6) {
        return false;
    }

    // Accept: Z, .sssZ, +HH:MM, -HH:MM, or no suffix.
    const char *tz = iso + n;
    if (*tz == '.') {
        ++tz;
        while (*tz >= '0' && *tz <= '9') {
            ++tz;
        }
    }

    int tzOffsetSec = 0;
    if (*tz == 'Z' || *tz == '\0') {
        // UTC or no explicit zone.
    } else if (*tz == '+' || *tz == '-') {
        const int sign = (*tz == '-') ? -1 : 1;
        ++tz;
        int tzh = 0;
        int tzm = 0;
        if (sscanf(tz, "%d:%d", &tzh, &tzm) == 2) {
            tzOffsetSec = sign * (tzh * 3600 + tzm * 60);
        } else if (sscanf(tz, "%2d%2d", &tzh, &tzm) == 2) {
            tzOffsetSec = sign * (tzh * 3600 + tzm * 60);
        } else {
            return false;
        }
    } else {
        return false;
    }

    const int64_t days = daysFromCivil(y, static_cast<unsigned>(mon), static_cast<unsigned>(day));
    int64_t sec = days * 86400LL + hh * 3600LL + mm * 60LL + ss;
    sec -= tzOffsetSec;
    epochOut = static_cast<time_t>(sec);
    return true;
}

static bool ensureUkTimeConfigured() {
    if (timeConfigured && time(nullptr) > 100000) {
        return true;
    }

    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

    const uint32_t startMs = millis();
    while (millis() - startMs < 10000) {
        if (time(nullptr) > 100000) {
            timeConfigured = true;
            Serial.println("NTP synced (UK timezone)");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    Serial.println("NTP sync timeout");
    return false;
}

static bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("WiFi connect timeout");
    return false;
}

static bool isDemoModeEnabled() {
    if (!demoMutex) {
        return false;
    }
    if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const bool enabled = demoState.enabled;
    xSemaphoreGive(demoMutex);
    return enabled;
}

static bool copyDemoState(DemoState &out) {
    if (!demoMutex) {
        return false;
    }
    if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    out = demoState;
    xSemaphoreGive(demoMutex);
    return true;
}

static void publishDemoStateToUi(const DemoState &state) {
    setSharedGwPoints(state.gwPoints);
    setSharedRankData(state.overallRank, state.rankDiff, state.hasRankData);
    setSharedTotalPoints(state.totalPoints, state.seeded);
    setSharedGameweekContext(state.isLiveGw, state.currentGw, state.nextGw, state.hasNextGw, state.deadlineUtc, state.hasDeadline);

    char gwStateBuf[48];
    snprintf(gwStateBuf, sizeof(gwStateBuf), "GW live: %s | next: %d", state.isLiveGw ? "yes" : "no", state.nextGw);
    setSharedGwStateText(gwStateBuf);
    setSharedFreshness(false, millis());
    updateSharedSquadFromPicks(state.picks, state.pickCount);
}

static bool parseIntToken(const char *token, int &out) {
    if (!token || !token[0]) {
        return false;
    }
    char *end = nullptr;
    const long parsed = strtol(token, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

static bool parseBoolToken(const char *token, bool &out) {
    if (!token) {
        return false;
    }
    if (strcmp(token, "1") == 0 || strcmp(token, "on") == 0 || strcmp(token, "true") == 0 || strcmp(token, "yes") == 0) {
        out = true;
        return true;
    }
    if (strcmp(token, "0") == 0 || strcmp(token, "off") == 0 || strcmp(token, "false") == 0 || strcmp(token, "no") == 0) {
        out = false;
        return true;
    }
    return false;
}

static void toLowerAscii(char *text) {
    if (!text) {
        return;
    }
    for (size_t i = 0; text[i]; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            text[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
}

static size_t splitTokens(char *line, char **tokens, size_t maxTokens) {
    if (!line || !tokens || maxTokens == 0) {
        return 0;
    }
    size_t count = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (count >= maxTokens) {
            break;
        }
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return count;
}

static TeamPick *findPickBySquadSlot(TeamPick *picks, size_t pickCount, int slot) {
    if (!picks || slot <= 0) {
        return nullptr;
    }
    for (size_t i = 0; i < pickCount; ++i) {
        if (picks[i].squadPosition == slot) {
            return &picks[i];
        }
    }
    if (slot <= static_cast<int>(pickCount)) {
        return &picks[slot - 1];
    }
    return nullptr;
}

static int canonicalPointsForLive(const TeamPick &pick, const TeamPick::LiveStats &live, bool &okOut) {
    TeamPick copy = pick;
    copy.live = live;
    const int oldTotal = copy.live.totalPoints;
    const int noBonus = computeExpectedPointsExcludingBonus(copy, okOut);
    if (!okOut) {
        return oldTotal;
    }
    return noBonus + copy.live.bonus;
}

static bool applyDemoEventToPick(TeamPick &pick, const char *eventType, int count, int &pointDeltaOut,
                                 const char *&labelOut) {
    if (!eventType || count <= 0) {
        return false;
    }

    TeamPick::LiveStats next = pick.live;
    const bool touchesPitchTime =
        strcmp(eventType, "bonus") != 0 && strcmp(eventType, "b") != 0 && strcmp(eventType, "defcontrib") != 0 &&
        strcmp(eventType, "dc") != 0 && strcmp(eventType, "minutes") != 0 && strcmp(eventType, "mins") != 0;
    if (touchesPitchTime && next.minutes < 1) {
        next.minutes = 1;
    }

    if (strcmp(eventType, "goal") == 0 || strcmp(eventType, "g") == 0) {
        next.goalsScored += count;
        labelOut = "GOAL!";
    } else if (strcmp(eventType, "assist") == 0 || strcmp(eventType, "a") == 0) {
        next.assists += count;
        labelOut = "ASSIST!";
    } else if (strcmp(eventType, "cs") == 0 || strcmp(eventType, "clean") == 0 || strcmp(eventType, "clean_sheet") == 0) {
        next.cleanSheets += count;
        if (next.minutes < 60) {
            next.minutes = 60;
        }
        labelOut = "CLEAN SHEET!";
    } else if (strcmp(eventType, "concede") == 0 || strcmp(eventType, "gc") == 0) {
        next.goalsConceded += count;
        labelOut = "goals against";
    } else if (strcmp(eventType, "save") == 0 || strcmp(eventType, "saves") == 0 || strcmp(eventType, "sv") == 0) {
        next.saves += count;
        labelOut = "SAVE BONUS!";
    } else if (strcmp(eventType, "bonus") == 0 || strcmp(eventType, "b") == 0) {
        next.bonus += count;
        labelOut = "BONUS PTS!";
    } else if (strcmp(eventType, "yc") == 0 || strcmp(eventType, "yellow") == 0) {
        next.yellowCards += count;
        labelOut = "YELLOW!";
    } else if (strcmp(eventType, "rc") == 0 || strcmp(eventType, "red") == 0) {
        next.redCards += count;
        labelOut = "RED!";
    } else if (strcmp(eventType, "og") == 0 || strcmp(eventType, "own_goal") == 0) {
        next.ownGoals += count;
        labelOut = "OWN GOAL!";
    } else if (strcmp(eventType, "pen_save") == 0 || strcmp(eventType, "psave") == 0) {
        next.penaltiesSaved += count;
        labelOut = "PEN SAVE!";
    } else if (strcmp(eventType, "pen_miss") == 0 || strcmp(eventType, "pmiss") == 0) {
        next.penaltiesMissed += count;
        labelOut = "PEN MISS!";
    } else if (strcmp(eventType, "defcontrib") == 0 || strcmp(eventType, "dc") == 0) {
        next.defensiveContributions += count;
        labelOut = "DEF CON!";
    } else if (strcmp(eventType, "minutes") == 0 || strcmp(eventType, "mins") == 0) {
        next.minutes += count;
        if (next.minutes < 0) {
            next.minutes = 0;
        }
        labelOut = "60+ mins!";
    } else {
        return false;
    }

    const int oldTotal = pick.live.totalPoints;
    next.totalPoints = oldTotal;
    bool prevOk = false;
    bool currOk = false;
    const int prevCanonical = canonicalPointsForLive(pick, pick.live, prevOk);
    const int currCanonical = canonicalPointsForLive(pick, next, currOk);
    pointDeltaOut = (prevOk && currOk) ? (currCanonical - prevCanonical) : 0;

    pick.live = next;
    pick.live.totalPoints = oldTotal + pointDeltaOut;
    return true;
}

static void printDemoStateSummary(const DemoState &state) {
    Serial.println("\n=== Demo Mode ===");
    Serial.printf("enabled: %s | seeded: %s\n", state.enabled ? "yes" : "no", state.seeded ? "yes" : "no");
    if (!state.seeded) {
        Serial.println("Run: demo seed");
        Serial.println("=================\n");
        return;
    }
    Serial.printf("GW%d points: %d | total: %d\n", state.currentGw, state.gwPoints, state.totalPoints);
    Serial.printf("GW live: %s | next: %d\n", state.isLiveGw ? "yes" : "no", state.nextGw);
    if (state.hasDeadline) {
        Serial.printf("deadline utc epoch: %lld\n", static_cast<long long>(state.deadlineUtc));
    } else {
        Serial.println("deadline: not set");
    }
    if (state.hasRankData) {
        Serial.printf("rank: %d (diff %+d)\n", state.overallRank, state.rankDiff);
    } else {
        Serial.println("rank: unavailable");
    }
    Serial.println("=================\n");
}

static void printDemoSquad(const DemoState &state) {
    if (!state.seeded) {
        Serial.println("[DEMO] Not seeded. Run: demo seed");
        return;
    }
    Serial.println("\n=== Demo Squad Slots ===");
    for (size_t i = 0; i < state.pickCount; ++i) {
        const TeamPick &p = state.picks[i];
        Serial.printf("slot:%2d | element:%4d | %-15s | %s | pts:%d | mult:%d%s%s\n",
                      p.squadPosition, p.elementId,
                      p.playerName.length() ? p.playerName.c_str() : "unknown",
                      p.teamShortName[0] ? p.teamShortName : "-",
                      p.live.totalPoints, p.multiplier, p.isCaptain ? " C" : "", p.isViceCaptain ? " VC" : "");
    }
    Serial.println("========================\n");
}

static void printDemoHelp() {
    Serial.println("\nDemo commands:");
    Serial.println("  demo help");
    Serial.println("  demo sequence <notification|deadline1|deadline2>");
    Serial.println("  demo seed");
    Serial.println("  demo on | demo off");
    Serial.println("  demo status | demo reset | demo squad");
    Serial.println("  gw live <0|1>");
    Serial.println("  gw current <num>");
    Serial.println("  gw next <num>");
    Serial.println("  gw deadline in <seconds>");
    Serial.println("  gw deadline clear");
    Serial.println("  event <slot> <type> [count]");
    Serial.println("Event types:");
    Serial.println("  goal assist cs concede save bonus yc rc og pen_save pen_miss defcontrib mins");
    Serial.println();
}

static void printDisplayHelp() {
    Serial.println("\nBetween-gameweeks screens (saved to flash):");
    Serial.println("  screens                       show current settings");
    Serial.println("  screens interval <seconds>    time per screen, 5-600");
    Serial.println("  screens graph <3|5|10|all>    gameweeks on the rank graph");
    Serial.println("  screens reset                 restore fpl_config.h defaults");
    Serial.println("  watch                         list watched players and their price status");
    Serial.println("  watch add <web name|id>       e.g. `watch add saka`, `watch add b.fernandes`");
    Serial.println("  watch remove <web name|id> | watch clear");
    Serial.println("  league                        list mini leagues (shown/hidden)");
    Serial.println("  league hide <id> | league show <id> | league show all");
    Serial.println();
}

static void printDisplaySettings() {
    static DisplaySettings settings;  // static: keeps it off the loop task stack
    displaySettingsGet(settings);
    Serial.printf("[SCREENS] interval %us | rank graph %s", settings.screenIntervalSec,
                  settings.rankGraphGameweeks == 0 ? "all GWs" : "");
    if (settings.rankGraphGameweeks != 0) {
        Serial.printf("last %u GWs", settings.rankGraphGameweeks);
    }
    Serial.printf(" | %u watched | %u hidden leagues\n", static_cast<unsigned>(settings.watchCount),
                  static_cast<unsigned>(settings.hiddenLeagueCount));
}

static void printWatchlist() {
    static DisplaySettings settings;
    IdleScreensData &data = *serialIdleData;
    displaySettingsGet(settings);
    copyIdleScreensDataIfChanged(data, 0);
    if (settings.watchCount == 0) {
        Serial.println("[WATCH] watchlist is empty (screen skipped). Add with `watch add <web name|id>`");
        return;
    }
    Serial.printf("[WATCH] %u watched:\n", static_cast<unsigned>(settings.watchCount));
    for (size_t i = 0; i < settings.watchCount; ++i) {
        const PlayerPrice *match = nullptr;
        for (size_t j = 0; j < data.prices.watchCount && !match; ++j) {
            if (watchEntryMatches(settings.watchlist[i], data.prices.watch[j].elementId, data.prices.watch[j].name)) {
                match = &data.prices.watch[j];
            }
        }
        if (!match) {
            Serial.printf("  %-20s (no data yet, or no player with this name)\n", settings.watchlist[i]);
            continue;
        }
        const int dir = expectedPriceDirection(*match);
        Serial.printf("  %-20s id %-4d %d.%dm  progress %+.1f%%  %s\n", match->name, match->elementId,
                      match->nowCost / 10, match->nowCost % 10, match->projectedPct / 10.0,
                      match->locked ? "locked" : (dir > 0 ? "RISE expected" : (dir < 0 ? "FALL expected" : "no change")));
    }
}

static void printLeagues() {
    static DisplaySettings settings;
    IdleScreensData &data = *serialIdleData;
    displaySettingsGet(settings);
    copyIdleScreensDataIfChanged(data, 0);
    if (!data.hasLeagues) {
        Serial.println("[LEAGUE] no data yet (waiting for the first FPL poll)");
        return;
    }
    if (data.leagueCount == 0) {
        Serial.println("[LEAGUE] not in any mini leagues (screen skipped)");
        return;
    }
    for (size_t i = 0; i < data.leagueCount; ++i) {
        const MiniLeague &lg = data.leagues[i];
        Serial.printf("  %-6s %8d  %-32s rank %d/%d (last %d)\n",
                      displaySettingsIsLeagueHidden(settings, lg.id) ? "hidden" : "shown", lg.id, lg.name, lg.rank,
                      lg.size, lg.lastRank);
    }
}

// Joins tokens[from..count) with single spaces, for names like "mac allister".
static void joinTokens(char **tokens, size_t from, size_t count, char *out, size_t outLen) {
    out[0] = '\0';
    for (size_t i = from; i < count; ++i) {
        if (i > from) {
            strlcat(out, " ", outLen);
        }
        strlcat(out, tokens[i], outLen);
    }
}

// Returns true if the line was a display settings command.
static bool handleDisplayCommand(char **tokens, size_t tokenCount) {
    if (strcmp(tokens[0], "screens") == 0) {
        if (tokenCount == 1) {
            printDisplaySettings();
        } else if (strcmp(tokens[1], "help") == 0) {
            printDisplayHelp();
        } else if (strcmp(tokens[1], "interval") == 0 && tokenCount >= 3) {
            int seconds = 0;
            if (!parseIntToken(tokens[2], seconds) || !displaySettingsSetScreenInterval(static_cast<uint16_t>(seconds))) {
                Serial.println("[SCREENS] interval must be 5-600 seconds");
                return true;
            }
            printDisplaySettings();
        } else if (strcmp(tokens[1], "graph") == 0 && tokenCount >= 3) {
            int gameweeks = 0;
            if (strcmp(tokens[2], "all") != 0 && !parseIntToken(tokens[2], gameweeks)) {
                gameweeks = -1;
            }
            if (gameweeks < 0 || gameweeks > 255 || !displaySettingsSetRankGraphGameweeks(static_cast<uint8_t>(gameweeks))) {
                Serial.println("[SCREENS] graph expects 3, 5, 10 or all");
                return true;
            }
            printDisplaySettings();
        } else if (strcmp(tokens[1], "reset") == 0) {
            displaySettingsReset();
            fplPollRequested = true;
            printDisplaySettings();
        } else {
            printDisplayHelp();
        }
        return true;
    }

    if (strcmp(tokens[0], "watch") == 0) {
        if (tokenCount == 1 || strcmp(tokens[1], "list") == 0) {
            printWatchlist();
            return true;
        }
        char entry[kWatchEntryLen];
        joinTokens(tokens, 2, tokenCount, entry, sizeof(entry));
        if (strcmp(tokens[1], "add") == 0 && entry[0]) {
            switch (displaySettingsWatchAdd(entry)) {
                case WatchlistResult::Ok:
                    Serial.printf("[WATCH] added '%s', fetching prices...\n", entry);
                    fplPollRequested = true;
                    break;
                case WatchlistResult::Full: Serial.printf("[WATCH] watchlist full (%u max)\n", static_cast<unsigned>(kMaxWatchlist)); break;
                case WatchlistResult::Duplicate: Serial.println("[WATCH] already watched"); break;
                default: Serial.println("[WATCH] invalid name"); break;
            }
        } else if (strcmp(tokens[1], "remove") == 0 && entry[0]) {
            if (displaySettingsWatchRemove(entry) == WatchlistResult::Ok) {
                Serial.printf("[WATCH] removed '%s'\n", entry);
                fplPollRequested = true;
            } else {
                Serial.println("[WATCH] not in watchlist (use the name exactly as `watch` lists it)");
            }
        } else if (strcmp(tokens[1], "clear") == 0) {
            displaySettingsWatchClear();
            fplPollRequested = true;
            Serial.println("[WATCH] cleared, watchlist screen will be skipped");
        } else {
            printDisplayHelp();
        }
        return true;
    }

    if (strcmp(tokens[0], "league") == 0 || strcmp(tokens[0], "leagues") == 0) {
        if (tokenCount == 1 || strcmp(tokens[1], "list") == 0) {
            printLeagues();
            return true;
        }
        const bool hide = strcmp(tokens[1], "hide") == 0;
        const bool show = strcmp(tokens[1], "show") == 0;
        if ((hide || show) && tokenCount >= 3) {
            if (show && strcmp(tokens[2], "all") == 0) {
                displaySettingsShowAllLeagues();
                Serial.println("[LEAGUE] all mini leagues shown");
                return true;
            }
            int id = 0;
            if (!parseIntToken(tokens[2], id) || !displaySettingsSetLeagueHidden(id, hide)) {
                Serial.println("[LEAGUE] expects a league ID from `league` (hidden list holds 32 max)");
                return true;
            }
            Serial.printf("[LEAGUE] %d %s\n", id, hide ? "hidden" : "shown");
            return true;
        }
        printDisplayHelp();
        return true;
    }
    return false;
}

static void handleSerialCommandLine(char *line) {
    if (!line) {
        return;
    }

    char *tokens[8] = {nullptr};
    const size_t tokenCount = splitTokens(line, tokens, sizeof(tokens) / sizeof(tokens[0]));
    if (tokenCount == 0) {
        return;
    }
    for (size_t i = 0; i < tokenCount; ++i) {
        toLowerAscii(tokens[i]);
    }

    if (strcmp(tokens[0], "help") == 0 || strcmp(tokens[0], "?") == 0) {
        printDemoHelp();
        printDisplayHelp();
        return;
    }
    if (handleDisplayCommand(tokens, tokenCount)) {
        return;
    }

    if (strcmp(tokens[0], "demo") == 0) {
        if (tokenCount < 2 || strcmp(tokens[1], "help") == 0) {
            printDemoHelp();
            return;
        }

        if (strcmp(tokens[1], "sequence") == 0) {
            if (tokenCount < 3) {
                Serial.println("[DEMO] Usage: demo sequence <notification|deadline1|deadline2>");
                return;
            }

            if (strcmp(tokens[2], "notification") == 0 || strcmp(tokens[2], "notify") == 0 ||
                strcmp(tokens[2], "notif") == 0 || strcmp(tokens[2], "n") == 0) {
                startNotificationScriptedDemo();
                Serial.println("[DEMO] Sequence started: notification");
                return;
            }
            if (strcmp(tokens[2], "deadline1") == 0 || strcmp(tokens[2], "d1") == 0 ||
                strcmp(tokens[2], "45m") == 0) {
                startDeadlineScriptedDemo(kDemoDeadline45mSec);
                Serial.println("[DEMO] Sequence started: deadline1 (45m)");
                return;
            }
            if (strcmp(tokens[2], "deadline2") == 0 || strcmp(tokens[2], "d2") == 0 ||
                strcmp(tokens[2], "7m") == 0) {
                startDeadlineScriptedDemo(kDemoDeadline7mSec);
                Serial.println("[DEMO] Sequence started: deadline2 (7m)");
                return;
            }

            Serial.println("[DEMO] Unknown sequence. Use: notification|deadline1|deadline2");
            return;
        }

        if (strcmp(tokens[1], "seed") == 0) {
            if (WiFi.status() != WL_CONNECTED && !connectWiFi()) {
                Serial.println("[DEMO] Seed failed: WiFi not connected");
                return;
            }
            ensureUkTimeConfigured();

            static TeamSnapshot snapshot;  // static: too large for the serial task stack
            if (!fetchTeamSnapshot(snapshot)) {
                Serial.println("[DEMO] Seed failed: could not fetch team snapshot");
                return;
            }

            const bool isLive = snapshot.isLiveGw;
            const int nextGw = snapshot.nextGw;
            const bool hasDeadline = snapshot.hasDeadline;
            const time_t deadlineUtc = snapshot.deadlineUtc;
            const int rank = snapshot.overallRank;
            const int rankDiff = snapshot.rankDiff;
            const bool hasRankData = snapshot.hasRankData;

            DemoState updated;
            if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
                Serial.println("[DEMO] Seed failed: demo state mutex timeout");
                return;
            }
            demoState.seeded = true;
            demoState.pickCount = snapshot.pickCount;
            for (size_t i = 0; i < snapshot.pickCount; ++i) {
                demoState.picks[i] = snapshot.picks[i];
                demoState.seededPicks[i] = snapshot.picks[i];
            }
            demoState.currentGw = snapshot.currentGw;
            demoState.gwPoints = snapshot.gwPoints;
            demoState.seededGwPoints = snapshot.gwPoints;
            demoState.totalPoints = snapshot.overallPoints;
            demoState.seededTotalPoints = snapshot.overallPoints;
            demoState.overallRank = rank;
            demoState.rankDiff = rankDiff;
            demoState.hasRankData = hasRankData;
            demoState.isLiveGw = isLive;
            demoState.nextGw = nextGw;
            demoState.hasNextGw = nextGw > 0;
            demoState.hasDeadline = hasDeadline;
            demoState.deadlineUtc = deadlineUtc;
            updated = demoState;
            xSemaphoreGive(demoMutex);

            clearUiEvents();
            publishDemoStateToUi(updated);
            setSharedStatus(updated.enabled ? "Demo mode active" : "Demo seeded (ready)", 0x00E5FF);
            printDemoStateSummary(updated);
            printDemoSquad(updated);
            return;
        }

        if (strcmp(tokens[1], "on") == 0) {
            DemoState updated;
            if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                Serial.println("[DEMO] Failed: demo state mutex timeout");
                return;
            }
            if (!demoState.seeded) {
                xSemaphoreGive(demoMutex);
                Serial.println("[DEMO] Run `demo seed` first");
                return;
            }
            demoState.enabled = true;
            updated = demoState;
            xSemaphoreGive(demoMutex);
            publishDemoStateToUi(updated);
            setSharedStatus("Demo mode active", 0x00E5FF);
            printDemoStateSummary(updated);
            return;
        }

        if (strcmp(tokens[1], "off") == 0) {
            if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                Serial.println("[DEMO] Failed: demo state mutex timeout");
                return;
            }
            demoState.enabled = false;
            xSemaphoreGive(demoMutex);
            setSharedStatus("Demo mode off (live polling)", 0xFFCC66);
            Serial.println("[DEMO] disabled, live polling resumed");
            return;
        }

        if (strcmp(tokens[1], "status") == 0) {
            DemoState local;
            if (!copyDemoState(local)) {
                Serial.println("[DEMO] Failed: unable to read demo state");
                return;
            }
            printDemoStateSummary(local);
            return;
        }

        if (strcmp(tokens[1], "squad") == 0) {
            DemoState local;
            if (!copyDemoState(local)) {
                Serial.println("[DEMO] Failed: unable to read demo state");
                return;
            }
            printDemoSquad(local);
            return;
        }

        if (strcmp(tokens[1], "reset") == 0) {
            DemoState updated;
            if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
                Serial.println("[DEMO] Failed: demo state mutex timeout");
                return;
            }
            if (!demoState.seeded) {
                xSemaphoreGive(demoMutex);
                Serial.println("[DEMO] Run `demo seed` first");
                return;
            }
            for (size_t i = 0; i < demoState.pickCount; ++i) {
                demoState.picks[i] = demoState.seededPicks[i];
            }
            demoState.gwPoints = demoState.seededGwPoints;
            demoState.totalPoints = demoState.seededTotalPoints;
            updated = demoState;
            xSemaphoreGive(demoMutex);

            clearUiEvents();
            publishDemoStateToUi(updated);
            setSharedStatus(updated.enabled ? "Demo mode active" : "Demo reset", 0x00E5FF);
            printDemoStateSummary(updated);
            return;
        }

        Serial.println("[DEMO] Unknown demo command. Try `demo help`");
        return;
    }

    if (strcmp(tokens[0], "gw") == 0) {
        if (tokenCount < 3) {
            Serial.println("[DEMO] Usage: gw live|current|next|deadline ...");
            return;
        }

        DemoState updated;
        if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            Serial.println("[DEMO] Failed: demo state mutex timeout");
            return;
        }
        if (!demoState.seeded || !demoState.enabled) {
            xSemaphoreGive(demoMutex);
            Serial.println("[DEMO] Requires active demo mode (run `demo seed`, `demo on`)");
            return;
        }

        bool changed = false;
        if (strcmp(tokens[1], "live") == 0) {
            bool isLive = false;
            if (!parseBoolToken(tokens[2], isLive)) {
                xSemaphoreGive(demoMutex);
                Serial.println("[DEMO] gw live expects 0|1");
                return;
            }
            demoState.isLiveGw = isLive;
            changed = true;
        } else if (strcmp(tokens[1], "current") == 0) {
            int gw = 0;
            if (!parseIntToken(tokens[2], gw) || gw <= 0) {
                xSemaphoreGive(demoMutex);
                Serial.println("[DEMO] gw current expects positive integer");
                return;
            }
            demoState.currentGw = gw;
            changed = true;
        } else if (strcmp(tokens[1], "next") == 0) {
            int gw = 0;
            if (!parseIntToken(tokens[2], gw) || gw <= 0) {
                xSemaphoreGive(demoMutex);
                Serial.println("[DEMO] gw next expects positive integer");
                return;
            }
            demoState.nextGw = gw;
            demoState.hasNextGw = true;
            changed = true;
        } else if (strcmp(tokens[1], "deadline") == 0) {
            if (strcmp(tokens[2], "clear") == 0) {
                demoState.hasDeadline = false;
                demoState.deadlineUtc = 0;
                changed = true;
            } else if (strcmp(tokens[2], "in") == 0 && tokenCount >= 4) {
                int sec = 0;
                if (!parseIntToken(tokens[3], sec) || sec < 0) {
                    xSemaphoreGive(demoMutex);
                    Serial.println("[DEMO] gw deadline in expects non-negative seconds");
                    return;
                }
                ensureUkTimeConfigured();
                const time_t now = time(nullptr);
                if (now <= 100000) {
                    xSemaphoreGive(demoMutex);
                    Serial.println("[DEMO] Time unavailable, cannot set deadline");
                    return;
                }
                demoState.deadlineUtc = now + sec;
                demoState.hasDeadline = true;
                changed = true;
            } else {
                xSemaphoreGive(demoMutex);
                Serial.println("[DEMO] Usage: gw deadline in <seconds> | gw deadline clear");
                return;
            }
        } else {
            xSemaphoreGive(demoMutex);
            Serial.println("[DEMO] Unknown gw command");
            return;
        }

        if (!changed) {
            xSemaphoreGive(demoMutex);
            Serial.println("[DEMO] No changes applied");
            return;
        }
        updated = demoState;
        xSemaphoreGive(demoMutex);

        publishDemoStateToUi(updated);
        setSharedStatus("Demo GW context updated", 0x00E5FF);
        printDemoStateSummary(updated);
        return;
    }

    if (strcmp(tokens[0], "event") == 0) {
        if (tokenCount < 3) {
            Serial.println("[DEMO] Usage: event <slot> <type> [count]");
            return;
        }

        int slot = 0;
        if (!parseIntToken(tokens[1], slot) || slot <= 0) {
            Serial.println("[DEMO] Slot must be a positive integer");
            return;
        }
        int count = 1;
        if (tokenCount >= 4 && (!parseIntToken(tokens[3], count) || count <= 0)) {
            Serial.println("[DEMO] Count must be a positive integer");
            return;
        }

        DemoState updated;
        TeamPick changedPick;
        int pointDelta = 0;
        const char *eventLabel = nullptr;
        if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            Serial.println("[DEMO] Failed: demo state mutex timeout");
            return;
        }
        if (!demoState.seeded || !demoState.enabled) {
            xSemaphoreGive(demoMutex);
            Serial.println("[DEMO] Requires active demo mode (run `demo seed`, `demo on`)");
            return;
        }

        TeamPick *pick = findPickBySquadSlot(demoState.picks, demoState.pickCount, slot);
        if (!pick) {
            xSemaphoreGive(demoMutex);
            Serial.println("[DEMO] Unknown slot. Use `demo squad` to list slots");
            return;
        }

        if (!applyDemoEventToPick(*pick, tokens[2], count, pointDelta, eventLabel)) {
            xSemaphoreGive(demoMutex);
            Serial.println("[DEMO] Unknown event type. Use `demo help`");
            return;
        }
        changedPick = *pick;
        demoState.gwPoints = computeGwPointsFromPicks(demoState.picks, demoState.pickCount);
        demoState.totalPoints = demoState.seededTotalPoints + (demoState.gwPoints - demoState.seededGwPoints);
        updated = demoState;
        xSemaphoreGive(demoMutex);

        if (pointDelta != 0) {
            notifyEvent(changedPick, pointDelta, eventLabel ? eventLabel : "event");
        } else {
            Serial.printf("[DEMO EVENT] %s | no immediate point change\n",
                          changedPick.playerName.length() ? changedPick.playerName.c_str() : "unknown");
        }
        publishDemoStateToUi(updated);
        setSharedStatus("Demo event applied", 0x00E5FF);
        Serial.printf("[DEMO EVENT] slot:%d %s x%d => %+d pts | GW%d total: %d\n", slot,
                      eventLabel ? eventLabel : tokens[2], count, pointDelta, updated.currentGw, updated.gwPoints);
        return;
    }

    Serial.println("[DEMO] Unknown command. Try `demo help`");
}

static void processSerialInput() {
    while (Serial.available() > 0) {
        const int raw = Serial.read();
        if (raw < 0) {
            return;
        }
        const char c = static_cast<char>(raw);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (serialLineLen > 0) {
                serialLineBuffer[serialLineLen] = '\0';
                handleSerialCommandLine(serialLineBuffer);
                serialLineLen = 0;
            }
            continue;
        }
        if (c == '\b' || c == 127) {
            if (serialLineLen > 0) {
                serialLineLen--;
            }
            continue;
        }
        if (serialLineLen + 1 >= kSerialLineMax) {
            serialLineLen = 0;
            Serial.println("[DEMO] Command too long");
            continue;
        }
        serialLineBuffer[serialLineLen++] = c;
    }
}

static UiMode determineAutoMode(const SharedUiState &state) {
    if (currentMode == UiMode::EventsList || currentMode == UiMode::Squad || currentMode == UiMode::EventPopup) {
        return currentMode;
    }
    if (state.isLiveGw) {
        return UiMode::Live;
    }
    if (!state.hasNextDeadline || state.nextDeadlineUtc <= 0) {
        return UiMode::Idle;
    }
    const time_t now = time(nullptr);
    if (now <= 100000) {
        return UiMode::Idle;
    }
    const int64_t diff = static_cast<int64_t>(state.nextDeadlineUtc) - static_cast<int64_t>(now);
    if (diff <= 3600) {
        return UiMode::FinalHour;
    }
    if (diff <= 6 * 3600) {
        return UiMode::Deadline;
    }
    return UiMode::Idle;
}

static void updateModeUi(const SharedUiState &state, const UiRuntimeState &runtime) {
    char buf[96];

    if (ui.statusLabel) {
        lv_label_set_text(ui.statusLabel, state.statusText);
        lv_obj_set_style_text_color(ui.statusLabel, lv_color_hex(state.statusColor), LV_PART_MAIN);
    }

    if (ui.idleRankArrow && ui.idleRankValue) {
        if (!state.hasRankData) {
            lv_label_set_text(ui.idleRankArrow, "-");
            lv_label_set_text(ui.idleRankValue, "--");
            lv_obj_set_style_text_color(ui.idleRankArrow, lv_color_hex(kColorTextSecondary), LV_PART_MAIN);
        } else {
            lv_label_set_text(ui.idleRankArrow, state.rankDiff > 0 ? "^" : (state.rankDiff < 0 ? "v" : "-"));
            lv_obj_set_style_text_color(ui.idleRankArrow,
                                        lv_color_hex(state.rankDiff > 0 ? kColorAccentGreen :
                                                     (state.rankDiff < 0 ? kColorAccentRed : kColorTextSecondary)),
                                        LV_PART_MAIN);
            formatNumberWithCommas(state.overallRank, buf, sizeof(buf));
            lv_label_set_text(ui.idleRankValue, buf);
        }
    }

    if (ui.idleGwPoints) {
        snprintf(buf, sizeof(buf), "GW%d: %d pts", state.currentGw, state.gwPoints);
        lv_label_set_text(ui.idleGwPoints, buf);
    }
    if (ui.idleTotalPoints) {
        if (state.hasTotalPoints) {
            char totalBuf[24];
            formatNumberWithCommas(state.totalPoints, totalBuf, sizeof(totalBuf));
            snprintf(buf, sizeof(buf), "%s total pts", totalBuf);
        } else {
            strlcpy(buf, "-- total pts", sizeof(buf));
        }
        lv_label_set_text(ui.idleTotalPoints, buf);
    }

    if (ui.deadlineCountdown && state.hasNextDeadline) {
        const time_t now = time(nullptr);
        int64_t sec = now > 100000 ? (static_cast<int64_t>(state.nextDeadlineUtc) - static_cast<int64_t>(now)) : 0;
        if (sec < 0) {
            sec = 0;
        }
        const int hours = static_cast<int>(sec / 3600);
        const int mins = static_cast<int>((sec % 3600) / 60);
        const int secs = static_cast<int>(sec % 60);
        if (millis() - lastDeadlineBlinkMs >= 500) {
            deadlineColonVisible = !deadlineColonVisible;
            lastDeadlineBlinkMs = millis();
        }
        snprintf(buf, sizeof(buf), "%02d%c%02d%c%02d", hours, deadlineColonVisible ? ':' : ' ', mins,
                 deadlineColonVisible ? ':' : ' ', secs);
        lv_label_set_text(ui.deadlineCountdown, buf);
        snprintf(buf, sizeof(buf), "Gameweek %d", state.hasNextGw ? state.nextGw : 0);
        lv_label_set_text(ui.deadlineMeta, buf);
    }

    if (ui.finalCountdown && state.hasNextDeadline) {
        const time_t now = time(nullptr);
        int64_t sec = now > 100000 ? (static_cast<int64_t>(state.nextDeadlineUtc) - static_cast<int64_t>(now)) : 0;
        if (sec < 0) {
            sec = 0;
        }
        if (sec > 3600) {
            sec = 3600;
        }
        snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>(sec / 60), static_cast<int>(sec % 60));
        lv_label_set_text(ui.finalCountdown, buf);
    }

    if (ui.liveTitle) {
        snprintf(buf, sizeof(buf), "GW%d LIVE", state.currentGw);
        lv_label_set_text(ui.liveTitle, buf);
    }
    if (ui.livePoints) {
        snprintf(buf, sizeof(buf), "%d\npoints", state.gwPoints);
        lv_label_set_text(ui.livePoints, buf);
    }
    if (ui.liveRank) {
        if (state.hasRankData) {
            char rankBuf[24];
            formatNumberWithCommas(state.overallRank, rankBuf, sizeof(rankBuf));
            snprintf(buf, sizeof(buf), "%s %s", state.rankDiff >= 0 ? "^" : "v", rankBuf);
            lv_label_set_text(ui.liveRank, buf);
            lv_obj_set_style_text_color(ui.liveRank, lv_color_hex(state.rankDiff >= 0 ? kColorAccentGreen : kColorAccentRed),
                                        LV_PART_MAIN);
        } else {
            lv_label_set_text(ui.liveRank, "live rank --");
        }
    }
    if (ui.liveDot) {
        const bool pulseOn = ((millis() / 750U) % 2U) == 0U;
        lv_obj_set_style_bg_color(ui.liveDot, lv_color_hex(state.isStale ? kColorAccentAmber : kColorAccentCyan), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ui.liveDot, state.isStale ? LV_OPA_80 : (pulseOn ? LV_OPA_COVER : LV_OPA_30), LV_PART_MAIN);
    }

    if (ui.liveTickerLabel) {
        if (runtime.recentEventCount == 0) {
            lv_label_set_text(ui.liveTickerLabel, "No events yet");
        } else {
            if (millis() - lastTickerRotateMs > 3000U) {
                tickerEventIndex = (tickerEventIndex + 1) % runtime.recentEventCount;
                lastTickerRotateMs = millis();
            }
            const UiEventItem &e = runtime.recentEvents[(runtime.recentEventCount - 1) - tickerEventIndex];
            snprintf(buf, sizeof(buf), "%s %s %+d", e.icon, e.player, e.delta);
            lv_label_set_text(ui.liveTickerLabel, buf);
        }
    }

    if (currentMode == UiMode::EventsList && renderedEventsVersion != runtime.eventVersion) {
        refreshEventsList(runtime);
        renderedEventsVersion = runtime.eventVersion;
    }
    if (currentMode == UiMode::Squad && renderedSquadVersion != runtime.squadVersion) {
        refreshSquadList(runtime);
        renderedSquadVersion = runtime.squadVersion;
    }
}

static void showPopupEvent(const UiEventItem &event) {
    if (ui.popupTitle) {
        lv_label_set_text(ui.popupTitle, event.label[0] ? event.label : "event");
        lv_obj_set_style_text_color(ui.popupTitle, lv_color_hex(event.delta >= 0 ? kColorAccentGreen : kColorAccentRed), LV_PART_MAIN);
    }
    if (ui.popupPlayer) {
        lv_label_set_text(ui.popupPlayer, event.player);
    }
    if (ui.popupDelta) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%+d pts", event.delta);
        lv_label_set_text(ui.popupDelta, buf);
        lv_obj_set_style_text_color(ui.popupDelta, lv_color_hex(event.delta >= 0 ? kColorAccentGreen : kColorAccentRed), LV_PART_MAIN);
    }
    if (ui.popupTotal) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%d -> %d total", event.totalBefore, event.totalAfter);
        lv_label_set_text(ui.popupTotal, buf);
    }
    if (ui.popupKit) {
        if (resolveAndLoadKitImage(event)) {
            lv_obj_clear_flag(ui.popupKit, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(ui.popupKit, &kitImageDsc);
        } else {
            lv_obj_add_flag(ui.popupKit, LV_OBJ_FLAG_HIDDEN);
        }
    }
    loadMode(UiMode::EventPopup, LV_SCR_LOAD_ANIM_FADE_ON);
    ledRingTriggerNotificationForMs(kPopupDisplayDurationMs);
    popupHideAtMs = millis() + kPopupDisplayDurationMs;
}

static void uiTask(void *) {
    SharedUiState localState;
    UiRuntimeState localRuntime;

    for (;;) {
        lv_timer_handler();
        if (readSharedUiState(localState)) {
            snapshotUiRuntime(localRuntime);
            updateModeUi(localState, localRuntime);
            serviceScriptedDemo();
            serviceIdleCarousel();

            UiMode autoMode = determineAutoMode(localState);
            if (currentMode != UiMode::EventsList && currentMode != UiMode::Squad && currentMode != UiMode::EventPopup) {
                if (autoMode != currentMode) {
                    loadMode(autoMode, LV_SCR_LOAD_ANIM_FADE_ON);
                }
            }

            if (currentMode == UiMode::Live) {
                UiEventItem popupEvent;
                if (popUiPopup(popupEvent)) {
                    showPopupEvent(popupEvent);
                }
            }
            if (currentMode == UiMode::EventPopup && popupHideAtMs > 0 && millis() >= popupHideAtMs) {
                popupHideAtMs = 0;
                loadMode(UiMode::Live, LV_SCR_LOAD_ANIM_FADE_ON);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void fplTask(void *) {
    lastPollMs = 0;
    lastWifiRetryMs = 0;
    uint32_t lastSuccessMs = 0;
    uint32_t pollDelayMs = 0;
    uint32_t lastSuccessPollDelayMs = FPL_POLL_INTERVAL_MS;
    bool demoModeAnnounced = false;
    static TeamSnapshot snapshot;  // static: keeps ~3 KB off the task stack
    setSharedStatus("Connecting WiFi...", 0xFFCC66);

    for (;;) {
        const uint32_t now = millis();

        if (isScriptedDemoActive()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (isDemoModeEnabled()) {
            if (!demoModeAnnounced) {
                setSharedStatus("Demo mode active", 0x00E5FF);
                setSharedFreshness(false, now);
                demoModeAnnounced = true;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        demoModeAnnounced = false;

        if (WiFi.status() != WL_CONNECTED) {
            if (lastWifiRetryMs == 0 || now - lastWifiRetryMs >= 10000) {
                lastWifiRetryMs = now;
                setSharedStatus("Reconnecting WiFi...", 0xFFCC66);
                if (connectWiFi()) {
                    ensureUkTimeConfigured();
                    setSharedStatus("WiFi connected", 0x38D39F);
                    pollDelayMs = 0;  // refresh as soon as we are back online
                } else {
                    setSharedStatus("WiFi not connected", 0xFF5A5A);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (lastPollMs == 0 || now - lastPollMs >= pollDelayMs || fplPollRequested) {
            fplPollRequested = false;
            lastPollMs = now;
            setSharedStatus("Fetching FPL points...", 0xFFCC66);

            FplPollResult result;
            bool ok = false;
            if (xSemaphoreTake(fplDataMutex, portMAX_DELAY) == pdTRUE) {
                ok = refreshFplData(result);
                if (ok) {
                    fillSnapshotFromCache(*fplCache, snapshot);
                    publishIdleScreensFromCache(*fplCache);
                }
                xSemaphoreGive(fplDataMutex);
            }

            if (ok) {
                pollDelayMs = result.nextPollDelayMs;
                lastSuccessPollDelayMs = pollDelayMs;

                if (snapshot.hasGwState) {
                    char gwStateBuf[48];
                    snprintf(gwStateBuf, sizeof(gwStateBuf), "GW live: %s | next: %d", snapshot.isLiveGw ? "yes" : "no",
                             snapshot.nextGw);
                    setSharedGwStateText(gwStateBuf);
                    Serial.printf("GW state: live=%s next=%d\n", snapshot.isLiveGw ? "yes" : "no", snapshot.nextGw);
                } else {
                    setSharedGwStateText("GW live: ? | next: --");
                }

                updateSharedSquadFromPicks(snapshot.picks, snapshot.pickCount);
                if (kUseServerEventBreakdown) {
                    detectAndNotifyPointChangesFromBreakdown(snapshot.currentGw, snapshot.picks, snapshot.pickCount);
                } else {
                    detectAndNotifyPointChanges(snapshot.currentGw, snapshot.picks, snapshot.pickCount);
                }
                if (result.liveFetched) {
                    printTeamSnapshot(snapshot);
                }

                setSharedGwPoints(snapshot.gwPoints);
                setSharedRankData(snapshot.overallRank, snapshot.rankDiff, snapshot.hasRankData);
                setSharedGameweekContext(snapshot.isLiveGw, snapshot.currentGw, snapshot.nextGw, snapshot.nextGw > 0,
                                         snapshot.deadlineUtc, snapshot.hasDeadline);
                setSharedTotalPoints(snapshot.overallPoints, true);
                lastSuccessMs = now;
                setSharedFreshness(false, lastSuccessMs);
                setSharedStatus("FPL updated", 0x38D39F);
                Serial.printf("FPL GW points: %d\n", snapshot.gwPoints);
            } else {
                pollDelayMs = FPL_POLL_INTERVAL_MS;
                // Idle polls are minutes apart, so "stale" is measured against the interval we expected.
                const bool stale = (lastSuccessMs == 0) || ((now - lastSuccessMs) > lastSuccessPollDelayMs + 300000U);
                setSharedFreshness(stale, lastSuccessMs);
                setSharedStatus("FPL fetch failed", 0xFF5A5A);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void ledTask(void *) {
    for (;;) {
        ledRingTick(millis());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    kitImageBuffer = static_cast<uint8_t *>(heap_caps_calloc(1, kKitRgb565Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    fplCache = psramNew<FplDataCache>();
    idleScreensData = psramNew<IdleScreensData>();
    idleUiData = psramNew<IdleScreensData>();
    serialIdleData = psramNew<IdleScreensData>();
    if (!kitImageBuffer || !fplCache || !idleScreensData || !idleUiData || !serialIdleData) {
        Serial.println("PSRAM allocation failed");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("\\n=== Waveshare ESP32-S3-Touch-LCD-1.46B: FPL Buddy ===");
    ledRingInit();

    if (!display.begin()) {
        Serial.println("Display init failed");
        while (true) {
            delay(1000);
        }
    }

    if (!touch.begin()) {
        Serial.println("Touch init failed (continuing without touch)");
    }

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed");
    } else {
        Serial.println("LittleFS mounted");
    }

    Serial.println("Init LVGL...");
    lv_init();
    lv_tick_set_cb(lvglTickCb);

    lvglBuf = static_cast<uint8_t *>(heap_caps_malloc(kLvglBufPixels * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (!lvglBuf) {
        Serial.println("LVGL DMA buffer allocation failed");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("Create LVGL display...");
    lvglDisp = lv_display_create(kDisplayWidth, kDisplayHeight);
    if (!lvglDisp) {
        Serial.println("LVGL display create failed");
        while (true) {
            delay(1000);
        }
    }
    lv_display_set_flush_cb(lvglDisp, lvglFlushCb);
    lv_display_set_buffers(lvglDisp, lvglBuf, nullptr, kLvglBufPixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

    Serial.println("Create LVGL input...");
    lv_indev_t *touchIndev = lv_indev_create();
    if (!touchIndev) {
        Serial.println("LVGL input create failed");
        while (true) {
            delay(1000);
        }
    }
    lv_indev_set_type(touchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchIndev, lvglTouchCb);

    Serial.println("Build UI...");
    createUi();
    lv_timer_handler();  // flush first frame before worker tasks start
    Serial.println("UI ready");
    sharedUiMutex = xSemaphoreCreateMutex();
    uiRuntimeMutex = xSemaphoreCreateMutex();
    demoMutex = xSemaphoreCreateMutex();
    scriptedDemoMutex = xSemaphoreCreateMutex();
    fplDataMutex = xSemaphoreCreateMutex();
    idleScreensMutex = xSemaphoreCreateMutex();
    displaySettingsInit();
    if (!sharedUiMutex) {
        Serial.println("Failed to create UI state mutex");
        while (true) {
            delay(1000);
        }
    }
    if (!uiRuntimeMutex) {
        Serial.println("Failed to create UI runtime mutex");
        while (true) {
            delay(1000);
        }
    }
    if (!demoMutex) {
        Serial.println("Failed to create demo state mutex");
        while (true) {
            delay(1000);
        }
    }
    if (!scriptedDemoMutex) {
        Serial.println("Failed to create scripted demo mutex");
        while (true) {
            delay(1000);
        }
    }
    if (!fplDataMutex || !idleScreensMutex) {
        Serial.println("Failed to create FPL data mutex");
        while (true) {
            delay(1000);
        }
    }
    configureFplHttp();
    setSharedStatus("Booting...", 0xA0A0A0);
    setSharedGwStateText("GW live: ? | next: --");
    setSharedGameweekContext(false, 0, 0, false, 0, false);
    setSharedRankData(0, 0, false);
    setSharedTotalPoints(0, false);
    setSharedFreshness(true, 0);

#if CONFIG_FREERTOS_UNICORE
    constexpr BaseType_t kUiCore = 0;
    constexpr BaseType_t kFplCore = 0;
#else
    constexpr BaseType_t kUiCore = 1;
    constexpr BaseType_t kFplCore = 0;
#endif

    xTaskCreatePinnedToCore(uiTask, "uiTask", 12288, nullptr, 2, &uiTaskHandle, kUiCore);
    xTaskCreatePinnedToCore(fplTask, "fplTask", 12288, nullptr, 1, &fplTaskHandle, kFplCore);
    xTaskCreatePinnedToCore(ledTask, "ledTask", 4096, nullptr, 2, &ledTaskHandle, kUiCore);

    if (!uiTaskHandle || !fplTaskHandle || !ledTaskHandle) {
        Serial.println("Failed to create worker tasks");
        while (true) {
            delay(1000);
        }
    }
    Serial.println("Worker tasks started");
    Serial.printf("Internal heap free %u, largest %u | PSRAM free %u\n",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    Serial.println("Type `demo help` in serial monitor for manual demo controls");
}

void loop() {
    processSerialInput();
    vTaskDelay(pdMS_TO_TICKS(20));
}
