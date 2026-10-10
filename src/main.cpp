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

#include "device_config.h"
#include "display_settings.h"
#include "fpl_config.h"
#include "led_ring.h"
#include "power.h"
#include "ui_kit.h"
#include "web_server.h"

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
// Two DMA render buffers: LVGL draws the next band while the previous one is being sent to the panel.
static constexpr size_t kLvglBufPixels = kDisplayWidth * 30;

static lv_display_t *lvglDisp = nullptr;
static uint8_t *lvglBuf = nullptr;
static uint8_t *lvglBuf2 = nullptr;

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
    Live,  // the live-gameweek carousel (LivePage)
    EventPopup
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

    // Idle (rank page)
    lv_obj_t *idleRankValue = nullptr;
    lv_obj_t *idleRankMove = nullptr;
    lv_obj_t *idleGwLabel = nullptr;
    lv_obj_t *idleGwPoints = nullptr;
    lv_obj_t *idleTotalPoints = nullptr;
    lv_obj_t *idleStatusDot = nullptr;
    lv_obj_t *idleDots = nullptr;

    // Deadline
    lv_obj_t *deadlineLabel = nullptr;
    lv_obj_t *deadlineCountdown = nullptr;
    lv_obj_t *deadlineMeta = nullptr;
    lv_obj_t *deadlineWhen = nullptr;

    // Final hour
    lv_obj_t *finalArc = nullptr;
    lv_obj_t *finalCountdown = nullptr;
    lv_obj_t *finalMeta = nullptr;

    // Live
    lv_obj_t *liveTitle = nullptr;
    lv_obj_t *liveDot = nullptr;
    lv_obj_t *livePoints = nullptr;
    lv_obj_t *liveRank = nullptr;
    lv_obj_t *liveRankMove = nullptr;
    lv_obj_t *liveTickerBtn = nullptr;  // latest-event card
    lv_obj_t *liveTickerLabel = nullptr;
    lv_obj_t *liveEventBadge = nullptr;
    lv_obj_t *liveEventDelta = nullptr;
    lv_obj_t *liveDots = nullptr;

    // Popup
    lv_obj_t *popupTitle = nullptr;
    lv_obj_t *popupKit = nullptr;
    lv_obj_t *popupPlayer = nullptr;
    lv_obj_t *popupDelta = nullptr;
    lv_obj_t *popupTotal = nullptr;

    // Shared top line / stale
    lv_obj_t *statusLabel = nullptr;

    // Lists (live carousel pages)
    lv_obj_t *eventsList = nullptr;
    lv_obj_t *eventsClearBtn = nullptr;
    lv_obj_t *eventsDots = nullptr;
    lv_obj_t *squadList = nullptr;
    lv_obj_t *squadDots = nullptr;
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
    bool matchInPlay = false;
    int nextGw = 0;
    bool hasDeadline = false;
    time_t deadlineUtc = 0;
    int gwPoints = 0;
    bool hasPlayerMeta = false;
    String activeChip;
    TeamPick picks[16];
    size_t pickCount = 0;
};

// Where the demo squad came from. None = seeded over serial (`demo seed`).
enum class DemoSource : uint8_t { None = 0, MyTeam, Sample };

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

    // Web demo (Demo tab of the web UI).
    DemoSource source = DemoSource::None;
    int pointsAdjust = 0;           // added to the squad's points, so the GW total can be set directly
    bool autoKickoff = true;        // the gameweek goes live when the deadline passes
    bool rankFollowsPoints = true;  // scoring events move the overall rank
    bool stale = false;             // show the data as stale (amber live dot, "FPL fetch failed")
    int screenRequest = -1;         // DemoScreen for uiTask to show; -1 = none
};

static LastPickState lastPickStates[16];
static DemoState demoState;
static SemaphoreHandle_t demoMutex = nullptr;
// demoState.enabled, readable without demoMutex (fplTask checks it while holding fplDataMutex).
static volatile bool demoEnabledFlag = false;
// Stops the between-gameweeks carousel advancing on its timer (web demo).
static volatile bool demoCarouselPaused = false;
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
static bool demoStop();

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
    char team[24] = "";    // kit slug, e.g. "man_city"; empty if unknown
    bool isGk = false;
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
    url += String(deviceConfigEntryId());
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
    url += String(deviceConfigEntryId());
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
    url += String(deviceConfigEntryId());
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

struct TeamSlug {
    int id = 0;
    char slug[24] = "";
};

static const char *teamSlugForId(const TeamSlug *teams, size_t teamCount, int teamId) {
    for (size_t i = 0; i < teamCount; ++i) {
        if (teams[i].id == teamId) {
            return teams[i].slug;
        }
    }
    return "";
}

static void fillPlayerPrice(JsonObject e, const char *asciiWebName, time_t nowUtc, const TeamSlug *teams,
                            size_t teamCount, PlayerPrice &out) {
    out = PlayerPrice{};
    out.elementId = e["id"] | 0;
    strlcpy(out.name, asciiWebName, sizeof(out.name));
    strlcpy(out.team, teamSlugForId(teams, teamCount, e["team"] | 0), sizeof(out.team));
    out.isGk = (e["element_type"] | 0) == 1;
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
    // Team and position are also needed for the kits on the price screens.
    elementFilter["element_type"] = true;
    elementFilter["team"] = true;
    JsonObject teamFilter = filter["teams"].createNestedObject();
    teamFilter["id"] = true;
    teamFilter["name"] = true;
    if (wantMeta) {
        JsonObject typeFilter = filter["element_types"].createNestedObject();
        typeFilter["id"] = true;
        typeFilter["singular_name_short"] = true;
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

    TeamSlug teamSlugs[24];
    size_t teamCount = 0;
#if FPL_ENABLE_NAME_LOOKUP
    for (JsonObject t : doc["teams"].as<JsonArray>()) {
        if (teamCount >= (sizeof(teamSlugs) / sizeof(teamSlugs[0]))) {
            break;
        }
        teamSlugs[teamCount].id = t["id"] | 0;
        slugifyTeamName(t["name"] | "", teamSlugs[teamCount].slug, sizeof(teamSlugs[teamCount].slug));
        normalizeKitTeamSlug(teamSlugs[teamCount].slug, sizeof(teamSlugs[teamCount].slug));
        ++teamCount;
    }
#endif

    bool watchFound[kMaxWatchlist] = {};
    PlayerPrice watchSlots[kMaxWatchlist];
    for (JsonObject e : doc["elements"].as<JsonArray>()) {
        const int id = e["id"] | 0;
        char asciiName[24];
        sanitizeUtf8ToAscii(e["web_name"] | "", asciiName, sizeof(asciiName));
        for (size_t i = 0; i < pickCount; ++i) {
            if (picks[i].elementId == id && prices.squadCount < 16) {
                fillPlayerPrice(e, asciiName, nowUtc, teamSlugs, teamCount, prices.squad[prices.squadCount++]);
                break;
            }
        }
        for (size_t i = 0; i < settings.watchCount; ++i) {
            if (!watchFound[i] && watchEntryMatches(settings.watchlist[i], id, asciiName)) {
                fillPlayerPrice(e, asciiName, nowUtc, teamSlugs, teamCount, watchSlots[i]);
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
            strlcpy(meta.teamShortName, teamSlugForId(teamSlugs, teamCount, meta.teamId), sizeof(meta.teamShortName));
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

// Queues a popup for a change of `pts` that took the player to `totalAfter` points.
static void pushPickEvent(const TeamPick &pick, int pts, const char *what, int totalAfter) {
    const char *name = pick.playerName.length() ? pick.playerName.c_str() : "unknown";
    Serial.printf("[FPL EVENT] %s %+d pt%s, %s\n", name, pts, (pts == 1 || pts == -1) ? "" : "s", what);

    UiEventItem event;
    strlcpy(event.icon, iconForEvent(what, pts), sizeof(event.icon));
    sanitizeUtf8ToAscii(what ? what : "event", event.label, sizeof(event.label));
    sanitizeUtf8ToAscii(name, event.player, sizeof(event.player));
    strlcpy(event.team, pick.teamShortName, sizeof(event.team));
    event.delta = pts;
    event.totalAfter = totalAfter;
    event.totalBefore = totalAfter - pts;
    event.isGk = (pick.elementType == 1);
    event.epochMs = millis();
    pushUiEvent(event);
}

static void notifyEvent(const TeamPick &pick, int pts, const char *what) {
    pushPickEvent(pick, pts, what, pick.live.totalPoints);
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

// Whether a fixture of the current GW is in play, and how many have finished. Without this GW's fixtures or a
// synced clock, falls back to treating the GW as in play (fetch live every poll).
static bool cacheMatchInPlay(const FplDataCache &c, time_t nowUtc, size_t &finishedFixtures) {
    const bool clockValid = nowUtc >= kMinValidEpoch;
    finishedFixtures = 0;
    if (c.fixturesGw != c.entry.currentGw) {
        return true;
    }
    bool matchInPlay = !clockValid;
    for (size_t i = 0; i < c.fixtureCount; ++i) {
        const FixtureWindow &f = c.fixtures[i];
        if (f.finished) {
            ++finishedFixtures;
        } else if (clockValid && f.hasKickoff && nowUtc >= f.kickoffUtc &&
                   nowUtc - f.kickoffUtc < kFixtureInPlayMaxSec) {
            matchInPlay = true;
        }
    }
    return matchInPlay;
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

    size_t finishedFixtures = 0;
    const bool matchInPlay = cacheMatchInPlay(c, nowUtc, finishedFixtures);
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
    // No previous rank before the first gameweek has finished: report no movement rather than a huge drop.
    out.rankDiff = out.hasRankData && c.entry.previousOverallRank > 0
                       ? c.entry.previousOverallRank - c.entry.overallRank
                       : 0;
    out.hasGwState = c.hasGwState;
    out.isLiveGw = c.gwState.isLiveGw;
    size_t finishedFixtures = 0;
    out.matchInPlay = cacheMatchInPlay(c, time(nullptr), finishedFixtures);
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
    // entry/{id}/ is fetched every poll, history only hourly: end the graph on the latest (during a gameweek, live)
    // overall rank.
    RankHistory &h = d.history;
    if (d.hasHistory && c.hasEntry && c.entry.overallRank > 0 && c.entry.currentGw > 0) {
        if (h.count > 0 && h.gw[h.count - 1] == c.entry.currentGw) {
            h.overallRank[h.count - 1] = c.entry.overallRank;
        } else if ((h.count == 0 || h.gw[h.count - 1] < c.entry.currentGw) && h.count < kMaxGameweeks) {
            h.gw[h.count] = c.entry.currentGw;
            h.overallRank[h.count] = c.entry.overallRank;
            ++h.count;
        }
    }
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
    Serial.printf("Entry ID: %d | GW: %d | GW points: %d\n", deviceConfigEntryId(), snapshot.currentGw, snapshot.gwPoints);
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

// Dev aid: the `screenshot` serial command redraws the whole display into this PSRAM frame and prints it as hex
// rows ("SS <row> <hex>" between "SS BEGIN w h" and "SS END"). UI task only, apart from the request fields.
static uint16_t *screenshotFrame = nullptr;
static bool screenshotCapturing = false;
static volatile bool screenshotRequested = false;
// IdlePage to show first, kIdlePageCount = events list, + 1 = squad, + 2 = squad scrolled to the end,
// -1 = the current screen.
static volatile int screenshotPage = -1;
static uint32_t screenshotAtMs = 0;

// The panel takes columns in groups of 4, so redraw areas are widened to 4-pixel boundaries.
static void lvglRounderCb(lv_event_t *e) {
    lv_area_t *area = static_cast<lv_area_t *>(lv_event_get_param(e));
    area->x1 &= ~0x3;
    area->x2 |= 0x3;
}

// Dev aid for the `fps` serial command: frame timings while the carousel animates to its next page.
static volatile bool fpsRequested = false;
static uint32_t fpsStartMs = 0;  // 0 = not measuring
static uint32_t fpsFrames = 0;
static uint32_t fpsFrameStartMs = 0;
static uint32_t fpsLongestMs = 0;
static uint32_t fpsFlushUs = 0;  // time spent handing bands to the panel (includes waiting for the previous one)
static uint32_t fpsFlushes = 0;
static volatile uint32_t fpsDmaQueuedUs = 0;
static volatile uint32_t fpsDmaUs = 0;     // queue-to-done time of the panel transfers
static volatile uint32_t fpsDmaBytes = 0;

static void lvglFrameTimingCb(lv_event_t *e) {
    if (fpsStartMs == 0) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        fpsFrameStartMs = millis();
        return;
    }
    const uint32_t frameMs = millis() - fpsFrameStartMs;
    fpsLongestMs = frameMs > fpsLongestMs ? frameMs : fpsLongestMs;
    ++fpsFrames;
}

// Runs from the SPI interrupt when the panel has taken a band: its buffer can be rendered into again.
static void lvglTransferDone(void *ctx) {
    if (fpsStartMs != 0) {
        fpsDmaUs += micros() - fpsDmaQueuedUs;
    }
    lv_display_flush_ready(static_cast<lv_display_t *>(ctx));
}

static void lvglFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int x1 = area->x1;
    const int y1 = area->y1;
    const int w = area->x2 - x1 + 1;
    const int h = area->y2 - y1 + 1;

    if (screenshotCapturing && screenshotFrame) {
        const uint16_t *src = reinterpret_cast<const uint16_t *>(px_map);
        for (int y = 0; y < h; ++y) {
            memcpy(screenshotFrame + (y1 + y) * kDisplayWidth + x1, src + y * w, static_cast<size_t>(w) * 2);
        }
    }
    // LVGL already renders in the panel's byte order (RGB565_SWAPPED), so the band goes out as is.
    const uint32_t flushStartUs = micros();
    fpsDmaQueuedUs = flushStartUs;
    fpsDmaBytes += static_cast<uint32_t>(w) * h * 2;
    const bool queued = display.drawBitmapAsync(x1, y1, w, h, px_map);
    if (fpsStartMs != 0) {
        fpsFlushUs += micros() - flushStartUs;
        ++fpsFlushes;
    }
    if (!queued) {
        lv_display_flush_ready(disp);
    }
}

static uint32_t lvglTickCb(void) {
    return millis();
}

// Current press, for the hold-to-enter-setup gesture (serviceSetupHold). UI task only.
// A press ends only after this many consecutive "not touched" reads spanning kTouchReleaseDebounceMs, so a
// dropped touch sample doesn't end it. Counting reads (not time since the last touch) matters: reads only
// happen between redraws, and a full-screen redraw takes longer than the debounce.
// Holding anywhere on the screen shows a countdown after kSetupHoldPromptMs; at kSetupHoldTriggerMs the device
// restarts into setup mode (or, when already in setup mode, restarts back to normal WiFi).
static constexpr uint32_t kSetupHoldPromptMs = 2000;
static constexpr uint32_t kSetupHoldTriggerMs = 7000;
// A press longer than this is not a tap, so letting go of a cancelled setup hold doesn't turn the page.
static constexpr uint32_t kTapMaxMs = 1000;

static constexpr uint32_t kTouchReleaseDebounceMs = 150;
static constexpr uint8_t kTouchReleaseReads = 3;
static uint32_t touchPressStartMs = 0;  // 0 = not pressed
static uint32_t touchReleasedSinceMs = 0;
static uint8_t touchReleasedReads = 0;
static uint32_t touchLastPressMs = 0;  // how long the last finished press lasted
static lv_point_t touchLastPoint = {0, 0};
// `device touch off`: ignore the touchscreen until restart (for screenshots, or a panel reporting ghost touches).
static volatile bool touchIgnored = false;

// LVGL gets the same debounced press: getTouch reports "not touched" when polled too soon after the last read or
// when a controller read fails, and passing that dropout on would make one tap two clicks (two page turns).
static void lvglTouchCb(lv_indev_t *, lv_indev_data_t *data) {
    if (touchIgnored) {
        touchPressStartMs = 0;
        touchReleasedReads = 0;
        data->point = touchLastPoint;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int x = 0;
    int y = 0;
    if (touch.getTouch(&x, &y)) {
        if (touchPressStartMs == 0) {
            touchPressStartMs = millis();
        }
        touchReleasedReads = 0;
        touchLastPoint.x = x;
        touchLastPoint.y = y;
    } else if (touchPressStartMs != 0) {
        const uint32_t now = millis();
        if (touchReleasedReads++ == 0) {
            touchReleasedSinceMs = now;
        } else if (touchReleasedReads >= kTouchReleaseReads && now - touchReleasedSinceMs >= kTouchReleaseDebounceMs) {
            touchLastPressMs = touchReleasedSinceMs - touchPressStartMs;
            if (touchLastPressMs >= kSetupHoldPromptMs) {
                Serial.printf("[SETUP] long press released after %u ms\n", static_cast<unsigned>(touchLastPressMs));
            }
            touchPressStartMs = 0;
            touchReleasedReads = 0;
        }
    }
    data->point = touchLastPoint;
    data->state = touchPressStartMs != 0 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// For LV_EVENT_CLICKED handlers: true unless the finger was down long enough to be a (cancelled) setup hold.
static bool isTap() {
    const uint32_t pressMs = touchPressStartMs != 0 ? millis() - touchPressStartMs : touchLastPressMs;
    return pressMs < kTapMaxMs;
}

static void formatNumberWithCommas(int value, char *out, size_t outLen) {
    uiFormatThousands(value, out, outLen);
}

static constexpr uint32_t kColorBgDeep = 0x1A0533;
static constexpr uint32_t kColorBgSurface = 0x2D1B4E;
static constexpr uint32_t kColorTextPrimary = 0xFFFFFF;
static constexpr uint32_t kColorTextSecondary = 0xB0A0C0;
static constexpr uint32_t kColorAccentGreen = 0x00FF87;
static constexpr uint32_t kColorAccentRed = 0xFF2882;
static constexpr uint32_t kColorAccentAmber = 0xFFC107;
static constexpr uint32_t kColorAccentCyan = 0x00E5FF;

static const lv_font_t *kFontHero = &lv_font_montserrat_48;
static const lv_font_t *kFontBody = &lv_font_montserrat_20;
static const lv_font_t *kFontCaption = &lv_font_montserrat_18;
static const lv_font_t *kFontMicro = &lv_font_montserrat_14;

static uint32_t popupHideAtMs = 0;
static constexpr uint32_t kPopupDisplayDurationMs = 4000U;
static uint32_t lastTickerRotateMs = 0;
static size_t tickerEventIndex = 0;
static uint32_t renderedEventsVersion = 0;
static uint32_t renderedSquadVersion = 0;

enum class IdlePage : uint8_t { Rank = 0, RankGraph, Prices, Watchlist, Leagues };
static constexpr uint8_t kIdlePageCount = 5;
static IdlePage idlePage = IdlePage::Rank;
static uint32_t idlePageShownMs = 0;
// List pages page through their rows before the carousel moves on. Shared by both carousels (only one shows).
static size_t carouselSubPage = 0;

// Live-gameweek carousel: the live score, the events and squad lists, and the between-gameweeks mini league
// and rank graph screens (showing live ranks), every screenIntervalSec or on tap.
enum class LivePage : uint8_t { Main = 0, Events, Squad, Leagues, RankGraph };
static constexpr uint8_t kLivePageCount = 5;
static LivePage livePage = LivePage::Main;
static uint32_t livePageShownMs = 0;

// The carousel on screen is the live one (the pop-up sits on top of it).
static bool inLiveCarousel() {
    return currentMode == UiMode::Live || currentMode == UiMode::EventPopup;
}

static lv_obj_t *livePageScreen(LivePage page) {
    switch (page) {
        case LivePage::Main: return ui.screenLive;
        case LivePage::Events: return ui.screenEvents;
        case LivePage::Squad: return ui.screenSquad;
        case LivePage::Leagues: return ui.screenIdleLeagues;
        case LivePage::RankGraph: return ui.screenIdleGraph;
    }
    return ui.screenLive;
}

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
        case UiMode::Live: return livePageScreen(livePage);
        case UiMode::EventPopup: return ui.screenPopup;
    }
    return ui.screenIdle;
}

static void renderIdlePage(IdlePage page, bool entering);
static void renderLivePage(LivePage page, bool entering);

static void loadMode(UiMode mode, lv_screen_load_anim_t anim) {
    if (currentMode == mode) {
        return;
    }
    const UiMode previous = currentMode;
    currentMode = mode;  // before rendering: the pages draw the dots of the carousel they belong to
    if (mode == UiMode::Idle) {
        // Entering the between-gameweeks carousel always starts on the rank page.
        idlePage = IdlePage::Rank;
        carouselSubPage = 0;
        idlePageShownMs = millis();
        renderIdlePage(idlePage, false);
    } else if (mode == UiMode::Live) {
        // The live carousel starts on the score, except when a pop-up hands back to the page it covered.
        if (previous != UiMode::EventPopup) {
            livePage = LivePage::Main;
            carouselSubPage = 0;
        }
        livePageShownMs = millis();
        renderLivePage(livePage, false);
    }
    lv_obj_t *target = modeToScreen(mode);
    if (target) {
        lv_screen_load_anim(target, anim, 200, 0, false);
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
    // No circular clip: the panel is round anyway, and clipping makes LVGL render every frame through a masked
    // layer (slow) and leaves the pixels outside the circle unpainted.
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

// Reads /kits/<team>_<type>_110x145.rgb565 (kKitRgb565Bytes) into dst.
static bool loadKitFile(const char *team, const char *type, uint8_t *dst) {
    if (!team || !team[0] || !type || !type[0] || !dst) {
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
    const size_t bytes = f.read(dst, kKitRgb565Bytes);
    f.close();
    if (bytes != kKitRgb565Bytes) {
        Serial.printf("[KIT] short read: %s (%u/%u)\n", path, static_cast<unsigned>(bytes),
                      static_cast<unsigned>(kKitRgb565Bytes));
        return false;
    }
    Serial.printf("[KIT] loaded: %s\n", path);
    return true;
}

// Goalkeepers fall back to the outfield kit when a team has no keeper kit file.
static bool loadTeamKit(const char *team, bool isGk, uint8_t *dst) {
    if (!team || !team[0]) {
        return false;
    }
    if (isGk) {
        return loadKitFile(team, "gk", dst) || loadKitFile(team, "goalkeeper", dst) ||
               loadKitFile(team, "outfield", dst);
    }
    return loadKitFile(team, "outfield", dst) || loadKitFile(team, "player", dst);
}

static bool resolveAndLoadKitImage(const UiEventItem &event) {
    if (!event.team[0]) {
        Serial.printf("[KIT] event missing team slug: player='%s' label='%s'\n", event.player, event.label);
        return false;
    }
    return loadTeamKit(event.team, event.isGk, kitImageBuffer);
}

// ---------------------------------------------------------------------------
// Between-gameweeks carousel
// Rank -> rank history -> squad price changes -> watchlist -> mini leagues, every screenIntervalSec or on tap.
// Pages with nothing to show are skipped (the rank page is always shown). List pages show up to kRowsPerPage
// rows and page through the rest before the carousel moves on. Built from the ui_kit components.
// ---------------------------------------------------------------------------
static constexpr size_t kRowsPerPage = 3;
static constexpr int kRowH = 78;
static constexpr int kRowGap = 10;
static constexpr int kRowBlockCy = 10;  // centre of the row block, from the screen centre
static constexpr int kRowMaxW = 300;
static constexpr int kRowPadX = 16;

// fplTask's status text after a successful poll; the rank page turns it into "Updated 5m ago".
static const char kStatusFplUpdated[] = "FPL updated";

static IdleScreensData *idleUiData = nullptr;      // PSRAM; uiTask's copy
static IdleScreensData *serialIdleData = nullptr;  // PSRAM; serial commands' copy
static DisplaySettings idleUiSettings;
static uint32_t idleUiDataVersion = 0;
static uint32_t idleUiSettingsVersion = 0;

static int64_t daysFromCivil(int y, unsigned m, unsigned d);

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

// 95 -> "£9.5m"
static void formatPrice(int tenths, char *out, size_t outLen) {
    snprintf(out, outLen, UI_POUND "%d.%dm", tenths / 10, tenths % 10);
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

// When the next price update happens, relative to today: "tonight 23:00", "tomorrow 23:00", "Sat 23:00".
// Empty when unknown. `tonightOut` is set when it happens later today.
static void formatNextPriceUpdate(char *out, size_t outLen, bool *tonightOut = nullptr) {
    out[0] = '\0';
    if (tonightOut) {
        *tonightOut = false;
    }
    const time_t now = time(nullptr);
    if (!idleUiData->prices.hasNextUpdate || now < kMinValidEpoch) {
        return;
    }
    const time_t t = idleUiData->prices.nextUpdateUtc;
    struct tm today;
    struct tm update;
    localtime_r(&now, &today);
    localtime_r(&t, &update);
    const int64_t days = daysFromCivil(update.tm_year + 1900, update.tm_mon + 1, update.tm_mday) -
                         daysFromCivil(today.tm_year + 1900, today.tm_mon + 1, today.tm_mday);
    static const char *const kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *day = kWeekdays[update.tm_wday % 7];
    if (days == 0) {
        day = update.tm_hour >= 17 ? "tonight" : "today";
    } else if (days == 1) {
        day = "tomorrow";
    }
    snprintf(out, outLen, "%s %02d:%02d", day, update.tm_hour, update.tm_min);
    if (tonightOut) {
        *tonightOut = days == 0;
    }
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

// Squad players expected to change price at the next update: rises first, strongest projection first.
static size_t squadPriceChanges(const PlayerPrice **out, size_t capacity) {
    const PriceData &prices = idleUiData->prices;
    size_t n = 0;
    for (size_t i = 0; i < prices.squadCount && n < capacity; ++i) {
        if (expectedPriceDirection(prices.squad[i]) != 0) {
            out[n++] = &prices.squad[i];
        }
    }
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && out[j]->projectedPct > out[j - 1]->projectedPct; --j) {
            const PlayerPrice *tmp = out[j - 1];
            out[j - 1] = out[j];
            out[j] = tmp;
        }
    }
    return n;
}

// Index range of the history entries shown on the graph (last N gameweeks, or all).
static size_t rankGraphFirstIndex() {
    const size_t count = idleUiData->history.count;
    const size_t wanted = idleUiSettings.rankGraphGameweeks;
    return (wanted == 0 || wanted >= count) ? 0 : count - wanted;
}

static size_t rowPages(size_t rows) {
    return rows <= 1 ? 1 : (rows + kRowsPerPage - 1) / kRowsPerPage;
}

// How many screens a carousel page takes (list pages show kRowsPerPage rows per screen).
static size_t idleSubPageCount(IdlePage page) {
    switch (page) {
        case IdlePage::Prices: {
            const PlayerPrice *changes[16];
            return rowPages(squadPriceChanges(changes, 16));
        }
        case IdlePage::Watchlist:
            return rowPages(idleUiData->prices.watchCount);
        case IdlePage::Leagues: {
            const MiniLeague *leagues[kMaxMiniLeagues];
            return rowPages(visibleLeagues(leagues, kMaxMiniLeagues));
        }
        case IdlePage::Rank:
        case IdlePage::RankGraph:
            break;
    }
    return 1;
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
        case IdlePage::Leagues: {
            const MiniLeague *leagues[kMaxMiniLeagues];
            return visibleLeagues(leagues, kMaxMiniLeagues) > 0;
        }
    }
    return false;
}

static bool livePageAvailable(LivePage page) {
    switch (page) {
        case LivePage::Leagues: return idlePageAvailable(IdlePage::Leagues);
        case LivePage::RankGraph: return idlePageAvailable(IdlePage::RankGraph);
        case LivePage::Main:
        case LivePage::Events:
        case LivePage::Squad:
            break;
    }
    return true;
}

static lv_obj_t *idlePageBody(IdlePage page) {
    switch (page) {
        case IdlePage::Rank: return ui.idleDots;
        case IdlePage::RankGraph: return ui.idleGraphBody;
        case IdlePage::Prices: return ui.idlePricesBody;
        case IdlePage::Watchlist: return ui.idleWatchBody;
        case IdlePage::Leagues: return ui.idleLeaguesBody;
    }
    return nullptr;
}

static void carouselTapEventCb(lv_event_t *);

// A carousel screen with a full-screen, non-clickable body that is rebuilt when its data changes (taps fall
// through to the screen).
static lv_obj_t *createIdleCarouselScreen(lv_obj_t *&bodyOut) {
    lv_obj_t *screen = lv_obj_create(nullptr);
    styleScreen(screen, kUiBg);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, carouselTapEventCb, LV_EVENT_CLICKED, nullptr);
    bodyOut = uiBox(screen, kUiScreenSize, kUiScreenSize);
    lv_obj_set_pos(bodyOut, 0, 0);
    return screen;
}

// Page dots for the pages currently in the carousel, with the one on screen highlighted.
static void addCarouselDots(lv_obj_t *parent) {
    uint8_t count = 0;
    uint8_t active = 0;
    if (inLiveCarousel()) {
        for (uint8_t i = 0; i < kLivePageCount; ++i) {
            const LivePage page = static_cast<LivePage>(i);
            if (!livePageAvailable(page)) {
                continue;
            }
            if (page == livePage) {
                active = count;
            }
            ++count;
        }
        uiPageDots(parent, count, active);
        return;
    }
    for (uint8_t i = 0; i < kIdlePageCount; ++i) {
        const IdlePage page = static_cast<IdlePage>(i);
        if (!idlePageAvailable(page)) {
            continue;
        }
        if (page == idlePage) {
            active = count;
        }
        ++count;
    }
    uiPageDots(parent, count, active);
}

// --- Kit thumbnails -------------------------------------------------------------------------------------------
// Kits are 110x145 RGB565 drawn on the screen background. For the price screens they're box-filtered down by an
// integer factor into RGB565A8 with the background keyed out, so they sit cleanly on card surfaces. The prices
// and watchlist pages use separate slots so a page sliding out keeps its kits while the next one renders.
static constexpr size_t kKitThumbSlots = 2 * kRowsPerPage;
static constexpr size_t kPricesKitSlot = 0;
static constexpr size_t kWatchKitSlot = kRowsPerPage;
static constexpr int kKitRowFactor = 3;        // 36x48 in list rows
static constexpr int kKitSpotlightFactor = 2;  // 55x72 when a page shows a single player

struct KitThumb {
    char key[40] = "";
    lv_image_dsc_t dsc = {};
    uint8_t *data = nullptr;  // PSRAM, sized for the smallest factor
};
static KitThumb kitThumbs[kKitThumbSlots];
static uint8_t *kitScratch = nullptr;  // PSRAM, kKitRgb565Bytes

static const lv_image_dsc_t *kitThumb(size_t slot, const char *team, bool isGk, int factor) {
    if (slot >= kKitThumbSlots || !team || !team[0] || factor < kKitSpotlightFactor) {
        return nullptr;
    }
    KitThumb &t = kitThumbs[slot];
    char key[sizeof(t.key)];
    snprintf(key, sizeof(key), "%s/%c/%d", team, isGk ? 'g' : 'o', factor);
    if (t.data && strcmp(t.key, key) == 0) {
        return &t.dsc;
    }
    static constexpr size_t kMaxThumbBytes =
        static_cast<size_t>(kKitWidth / kKitSpotlightFactor) * (kKitHeight / kKitSpotlightFactor) * 3;
    if (!kitScratch) {
        kitScratch = static_cast<uint8_t *>(heap_caps_malloc(kKitRgb565Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!t.data) {
        t.data = static_cast<uint8_t *>(heap_caps_malloc(kMaxThumbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    t.key[0] = '\0';
    if (!kitScratch || !t.data || !loadTeamKit(team, isGk, kitScratch)) {
        return nullptr;
    }

    const int w = kKitWidth / factor;
    const int h = kKitHeight / factor;
    const uint16_t *src = reinterpret_cast<const uint16_t *>(kitScratch);
    const uint16_t background = src[0];  // the corner pixel is always background
    uint16_t *rgb = reinterpret_cast<uint16_t *>(t.data);
    uint8_t *alpha = t.data + static_cast<size_t>(w) * h * 2;
    const int area = factor * factor;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            int opaque = 0;
            for (int dy = 0; dy < factor; ++dy) {
                const uint16_t *line = src + (y * factor + dy) * kKitWidth + x * factor;
                for (int dx = 0; dx < factor; ++dx) {
                    const uint16_t v = line[dx];
                    if (v == background) {
                        continue;
                    }
                    r += v >> 11;
                    g += (v >> 5) & 0x3F;
                    b += v & 0x1F;
                    ++opaque;
                }
            }
            const size_t i = static_cast<size_t>(y) * w + x;
            if (opaque == 0) {
                rgb[i] = 0;
                alpha[i] = 0;
                continue;
            }
            rgb[i] = static_cast<uint16_t>(((r / opaque) << 11) | ((g / opaque) << 5) | (b / opaque));
            alpha[i] = static_cast<uint8_t>(opaque * 255 / area);
        }
    }
    memset(&t.dsc, 0, sizeof(t.dsc));
    t.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    t.dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    t.dsc.header.w = w;
    t.dsc.header.h = h;
    t.dsc.header.stride = w * 2;
    t.dsc.data_size = static_cast<uint32_t>(w) * h * 3;
    t.dsc.data = t.data;
    strlcpy(t.key, key, sizeof(t.key));
    return &t.dsc;
}

// --- Shared row layouts ---------------------------------------------------------------------------------------

// Width shared by all rows of a page: as wide as the circle allows at the outermost row, capped.
static int rowWidthFor(size_t rows) {
    const int outer = uiRowCenter(rows - 1, rows, kRowH, kRowGap, kRowBlockCy);
    const int first = uiRowCenter(0, rows, kRowH, kRowGap, kRowBlockCy);
    const int edgeCy = (outer > -first) ? outer : first;
    // Rounded card corners are inset, so measure a little inside the card's top/bottom edge.
    const int w = uiSafeWidth(edgeCy, kRowH - 12, 8);
    return w < kRowMaxW ? w : kRowMaxW;
}

// A row inside a card: children laid out left to right, first and last pushed to the edges.
static lv_obj_t *createRowLine(lv_obj_t *row, int x, int w, lv_align_t align, int y) {
    lv_obj_t *line = uiBox(row, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, 10, LV_PART_MAIN);
    lv_obj_align(line, align, x, y);
    return line;
}

// Name label that takes the free space in a row line and truncates with "..." (never shrinks).
static lv_obj_t *createGrowingName(lv_obj_t *line, const lv_font_t *font, const char *text) {
    lv_obj_t *name = uiLabel(line, font, kUiTextPrimary, LV_TEXT_ALIGN_LEFT);
    uiSingleLine(name);
    lv_label_set_text(name, text);
    lv_obj_set_flex_grow(name, 1);
    return name;
}

struct PlayerLine {
    const char *value;    // e.g. "£9.5m"
    uint32_t valueColor;
    const char *pill;     // e.g. "LIKELY RISE"
    UiTone tone;
    const char *detail;   // secondary, e.g. "+87%"
};

//  [kit]  Saka               £9.5m
//         [LIKELY RISE]       +87%
static void createPlayerRow(lv_obj_t *body, int cy, int w, const PlayerPrice &p, size_t kitSlot,
                            const PlayerLine &line) {
    lv_obj_t *row = uiListRow(body, cy, w, kRowH);
    int contentX = 18;
    if (const lv_image_dsc_t *kit = kitThumb(kitSlot, p.team, p.isGk, kKitRowFactor)) {
        lv_obj_t *img = lv_image_create(row);
        lv_image_set_src(img, kit);
        lv_obj_align(img, LV_ALIGN_LEFT_MID, 12, 0);
        contentX = 12 + static_cast<int>(kit->header.w) + 10;
    }
    const int contentW = w - contentX - kRowPadX;

    lv_obj_t *top = createRowLine(row, contentX, contentW, LV_ALIGN_TOP_LEFT, 10);
    createGrowingName(top, UI_FONT_NAME, p.name);
    lv_obj_t *value = uiLabel(top, UI_FONT_NAME, line.valueColor, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(value, line.value);

    lv_obj_t *bottom = createRowLine(row, contentX, contentW, LV_ALIGN_BOTTOM_LEFT, -10);
    uiStatusPill(bottom, line.pill, line.tone);
    lv_obj_t *detail = uiLabel(bottom, UI_FONT_META, kUiTextSecondary, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(detail, line.detail);
}

// A page with one player gets the whole screen: kit, name, price, status.
static void createPlayerSpotlight(lv_obj_t *body, const PlayerPrice &p, size_t kitSlot, const char *priceLine,
                                  const PlayerLine &line) {
    if (const lv_image_dsc_t *kit = kitThumb(kitSlot, p.team, p.isGk, kKitSpotlightFactor)) {
        lv_obj_t *img = lv_image_create(body);
        lv_image_set_src(img, kit);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, -74);
    }
    lv_obj_t *name = uiLabel(body, UI_FONT_VALUE, kUiTextPrimary);
    lv_obj_set_width(name, uiSafeWidth(-14, 36));
    uiSingleLine(name);
    lv_label_set_text(name, p.name);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *price = uiLabel(body, UI_FONT_BODY, line.valueColor);
    lv_label_set_text(price, priceLine);
    lv_obj_align(price, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *pill = uiStatusPill(body, line.pill, line.tone);
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 56);

    if (line.detail && line.detail[0]) {
        lv_obj_t *detail = uiLabel(body, UI_FONT_META, kUiTextSecondary);
        lv_label_set_text(detail, line.detail);
        lv_obj_align(detail, LV_ALIGN_CENTER, 0, 92);
    }
}

// Clamps the list page shown to the pages available; returns the first row index on it.
static size_t currentRowPage(size_t rows, size_t &pagesOut) {
    pagesOut = rowPages(rows);
    if (carouselSubPage >= pagesOut) {
        carouselSubPage = 0;
    }
    return carouselSubPage * kRowsPerPage;
}

// "Next update tonight 23:00 · 1/2"
static void formatListSubtitle(char *out, size_t outLen, const char *text, size_t pages) {
    if (pages > 1) {
        snprintf(out, outLen, "%s%s%u/%u", text, text[0] ? "  " UI_DOT "  " : "",
                 static_cast<unsigned>(carouselSubPage + 1), static_cast<unsigned>(pages));
    } else {
        strlcpy(out, text, outLen);
    }
}

// --- Rank (home) page -----------------------------------------------------------------------------------------
//        OVERALL RANK
//          538,544
//         [▲ 54,201]
//     GW5    |   TOTAL
//      51    |    355
//      ● Updated 2m ago
static void createHomeScreen() {
    lv_obj_t *s = ui.screenIdle;
    uiScreenHeader(s, "OVERALL RANK");

    ui.idleRankValue = uiLabel(s, UI_FONT_HERO, kUiTextPrimary);
    uiNumberInit(ui.idleRankValue);
    lv_label_set_text(ui.idleRankValue, "--");
    lv_obj_align(ui.idleRankValue, LV_ALIGN_CENTER, 0, -84);

    ui.idleRankMove = uiStatusPill(s, "", UiTone::Neutral, UI_FONT_NAME);
    lv_obj_align(ui.idleRankMove, LV_ALIGN_CENTER, 0, -30);
    lv_obj_add_flag(ui.idleRankMove, LV_OBJ_FLAG_HIDDEN);

    static constexpr int kColumnX = 76;
    lv_obj_t *divider = uiBox(s, 1, 48);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kUiSurfaceRaised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(divider, LV_ALIGN_CENTER, 0, 36);

    ui.idleGwLabel = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_set_style_text_letter_space(ui.idleGwLabel, 1, LV_PART_MAIN);
    lv_label_set_text(ui.idleGwLabel, "GW");
    lv_obj_align(ui.idleGwLabel, LV_ALIGN_CENTER, -kColumnX, 20);
    lv_obj_t *totalLabel = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_set_style_text_letter_space(totalLabel, 1, LV_PART_MAIN);
    lv_label_set_text(totalLabel, "TOTAL");
    lv_obj_align(totalLabel, LV_ALIGN_CENTER, kColumnX, 20);

    ui.idleGwPoints = uiLabel(s, UI_FONT_VALUE, kUiTextPrimary);
    uiNumberInit(ui.idleGwPoints);
    lv_label_set_text(ui.idleGwPoints, "--");
    lv_obj_align(ui.idleGwPoints, LV_ALIGN_CENTER, -kColumnX, 50);
    ui.idleTotalPoints = uiLabel(s, UI_FONT_VALUE, kUiTextPrimary);
    uiNumberInit(ui.idleTotalPoints);
    lv_label_set_text(ui.idleTotalPoints, "--");
    lv_obj_align(ui.idleTotalPoints, LV_ALIGN_CENTER, kColumnX, 50);

    lv_obj_t *status = uiBox(s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 8, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 112);
    ui.idleStatusDot = uiDot(status, 8, kUiAccent);
    ui.statusLabel = uiLabel(status, UI_FONT_META, kUiTextSecondary);
    lv_label_set_text(ui.statusLabel, "");

    ui.idleDots = uiBox(s, kUiScreenSize, kUiScreenSize);
    lv_obj_set_pos(ui.idleDots, 0, 0);
}

// "Updated 5m ago" after a successful poll; otherwise fplTask's status (connecting, fetch failed, ...).
static void formatHomeStatus(const SharedUiState &state, char *out, size_t outLen, uint32_t &dotColor) {
    if (strcmp(state.statusText, kStatusFplUpdated) != 0 || state.lastApiUpdateMs == 0) {
        strlcpy(out, state.statusText, outLen);
        dotColor = state.statusColor;
        return;
    }
    dotColor = kUiAccent;
    const uint32_t minutes = (millis() - state.lastApiUpdateMs) / 60000U;
    if (minutes < 1) {
        strlcpy(out, "Updated now", outLen);
    } else if (minutes < 60) {
        snprintf(out, outLen, "Updated %um ago", static_cast<unsigned>(minutes));
    } else {
        snprintf(out, outLen, "Updated %uh ago", static_cast<unsigned>(minutes / 60));
    }
}

// Called every UI loop; the widgets only redraw when something they show changes.
static void updateHomeScreen(const SharedUiState &state) {
    if (!ui.idleRankValue) {
        return;
    }
    char buf[48];
    if (state.hasRankData) {
        uiNumberSet(ui.idleRankValue, state.overallRank, true);
        char diffBuf[24];
        formatNumberWithCommas(state.rankDiff < 0 ? -state.rankDiff : state.rankDiff, diffBuf, sizeof(diffBuf));
        if (state.rankDiff > 0) {
            snprintf(buf, sizeof(buf), UI_SYM_UP "  %s", diffBuf);
            uiStatusPillSet(ui.idleRankMove, buf, UiTone::Positive);
        } else if (state.rankDiff < 0) {
            snprintf(buf, sizeof(buf), UI_SYM_DOWN "  %s", diffBuf);
            uiStatusPillSet(ui.idleRankMove, buf, UiTone::Negative);
        } else {
            uiStatusPillSet(ui.idleRankMove, UI_DASH "  no change", UiTone::Neutral);
        }
        lv_obj_remove_flag(ui.idleRankMove, LV_OBJ_FLAG_HIDDEN);
    } else {
        uiNumberClear(ui.idleRankValue, "--");
        lv_obj_add_flag(ui.idleRankMove, LV_OBJ_FLAG_HIDDEN);
    }

    if (state.currentGw > 0) {
        snprintf(buf, sizeof(buf), "GW%d", state.currentGw);
    } else {
        strlcpy(buf, "GW", sizeof(buf));
    }
    uiSetText(ui.idleGwLabel, buf);
    if (state.hasGwPoints) {
        uiNumberSet(ui.idleGwPoints, state.gwPoints, true);
    } else {
        uiNumberClear(ui.idleGwPoints, "--");
    }
    if (state.hasTotalPoints) {
        uiNumberSet(ui.idleTotalPoints, state.totalPoints, true);
    } else {
        uiNumberClear(ui.idleTotalPoints, "--");
    }

    uint32_t dotColor = kUiAccent;
    formatHomeStatus(state, buf, sizeof(buf), dotColor);
    uiSetText(ui.statusLabel, buf);
    static uint32_t shownDotColor = 0;
    if (dotColor != shownDotColor) {
        lv_obj_set_style_bg_color(ui.idleStatusDot, lv_color_hex(dotColor), LV_PART_MAIN);
        shownDotColor = dotColor;
    }

    // New data that changed something: a brief glow around the display edge.
    static uint32_t seenUpdateMs = 0;
    static bool seenData = false;
    static int seenRank = 0;
    static int seenGwPoints = 0;
    static int seenTotal = 0;
    if (state.lastApiUpdateMs != seenUpdateMs) {
        const bool changed = seenData && (state.overallRank != seenRank || state.gwPoints != seenGwPoints ||
                                          state.totalPoints != seenTotal);
        if (changed && currentMode == UiMode::Idle) {
            uiPulseRing();
        }
        seenUpdateMs = state.lastApiUpdateMs;
        seenData = state.hasRankData;
        seenRank = state.overallRank;
        seenGwPoints = state.gwPoints;
        seenTotal = state.totalPoints;
    }
}

static void renderHomeDots() {
    if (ui.idleDots) {
        lv_obj_clean(ui.idleDots);
        addCarouselDots(ui.idleDots);
    }
}

// --- Rank history ---------------------------------------------------------------------------------------------
static void renderRankGraphPage(bool entering) {
    lv_obj_t *body = ui.idleGraphBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    const RankHistory &h = idleUiData->history;
    const size_t first = rankGraphFirstIndex();
    const size_t n = h.count > first ? h.count - first : 0;

    char buf[40];
    if (idleUiSettings.rankGraphGameweeks == 0) {
        strlcpy(buf, "All gameweeks", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "Last %u gameweeks", static_cast<unsigned>(n));
    }
    const bool live = inLiveCarousel();  // the last point is the live overall rank (publishIdleScreensFromCache)
    uiScreenHeader(body, live ? "LIVE RANK" : "RANK HISTORY", buf);
    addCarouselDots(body);
    if (n < 2) {
        uiEmptyState(body, UI_SYM_ARROW, "Not enough data yet", "The graph starts after two gameweeks",
                     UiTone::Neutral);
        return;
    }

    // Y axis: best (lowest) rank at the top, a small margin at both ends.
    int best = h.overallRank[first];
    int worst = h.overallRank[first];
    for (size_t i = first; i < h.count; ++i) {
        best = h.overallRank[i] < best ? h.overallRank[i] : best;
        worst = h.overallRank[i] > worst ? h.overallRank[i] : worst;
    }
    const int64_t margin = (static_cast<int64_t>(worst) - best) / 12 + 1;
    const int64_t axisTop = best - margin;
    const int64_t axisBottom = worst + margin;
    const int64_t span = axisBottom - axisTop;

    static constexpr int kChartX = 56;
    static constexpr int kChartY = 98;
    static constexpr int kChartW = 300;
    static constexpr int kChartH = 134;
    static constexpr int kChartPad = 4;
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
    lv_obj_set_style_line_width(chart, 4, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(chart, true, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_point_count(chart, static_cast<uint32_t>(n));
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, kScale);
    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(kUiAccent), LV_CHART_AXIS_PRIMARY_Y);
    int32_t firstValue = 0;
    int32_t lastValue = 0;
    for (size_t i = 0; i < n; ++i) {
        const int64_t fromBottom = axisBottom - h.overallRank[first + i];
        const int32_t value = static_cast<int32_t>(fromBottom * kScale / span);
        lv_chart_set_value_by_id(chart, series, static_cast<uint32_t>(i), value);
        firstValue = i == 0 ? value : firstValue;
        lastValue = value;
    }
    lv_chart_refresh(chart);

    // Start and end markers: the end point is the one that matters.
    const int contentH = kChartH - 2 * kChartPad;
    auto pointY = [&](int32_t value) {
        return kChartY + kChartPad + contentH - static_cast<int>(static_cast<int64_t>(value) * contentH / kScale);
    };
    lv_obj_t *startDot = uiDot(body, 8, kUiTextSecondary);
    lv_obj_set_pos(startDot, kChartX + kChartPad - 4, pointY(firstValue) - 4);
    lv_obj_t *endDot = uiDot(body, 16, kUiAccent);
    lv_obj_set_style_border_color(endDot, lv_color_hex(kUiBg), LV_PART_MAIN);
    lv_obj_set_style_border_width(endDot, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(endDot, lv_color_hex(kUiAccent), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(endDot, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(endDot, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_pos(endDot, kChartX + kChartW - kChartPad - 8, pointY(lastValue) - 8);

    // Only the ends of the time axis are labelled.
    lv_obj_t *startGw = uiLabel(body, UI_FONT_META, kUiTextSecondary, LV_TEXT_ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "GW%d", h.gw[first]);
    lv_label_set_text(startGw, buf);
    lv_obj_set_pos(startGw, kChartX, kChartY + kChartH + 8);
    lv_obj_t *endGw = uiLabel(body, UI_FONT_META, kUiTextSecondary, LV_TEXT_ALIGN_RIGHT);
    snprintf(buf, sizeof(buf), live ? "GW%d live" : "GW%d", h.gw[h.count - 1]);
    lv_label_set_text(endGw, buf);
    lv_obj_align(endGw, LV_ALIGN_TOP_RIGHT, -(kUiScreenSize - kChartX - kChartW), kChartY + kChartH + 8);

    const int latest = h.overallRank[h.count - 1];
    const int start = h.overallRank[first];
    lv_obj_t *rank = uiLabel(body, UI_FONT_VALUE, kUiTextPrimary);
    formatNumberWithCommas(latest, buf, sizeof(buf));
    lv_label_set_text(rank, buf);
    lv_obj_align(rank, LV_ALIGN_CENTER, 0, 80);

    char change[16];
    formatRankCompact(start > latest ? start - latest : latest - start, change, sizeof(change));
    if (latest < start) {
        snprintf(buf, sizeof(buf), UI_SYM_UP " %s since GW%d", change, h.gw[first]);
    } else if (latest > start) {
        snprintf(buf, sizeof(buf), UI_SYM_DOWN " %s since GW%d", change, h.gw[first]);
    } else {
        snprintf(buf, sizeof(buf), "Level since GW%d", h.gw[first]);
    }
    lv_obj_t *pill = uiStatusPill(body, buf,
                                  latest < start ? UiTone::Positive : (latest > start ? UiTone::Negative : UiTone::Neutral));
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 118);

    if (entering) {
        // Draw the line left to right once the page has slid in.
        uiRevealLeftToRight(body, kChartX - 14, kChartY - 14, kChartW + 30, kChartH + 20, 700, kUiMotionMs);
    }
}

// --- Price changes (my squad) ---------------------------------------------------------------------------------
static void renderPricesPage() {
    lv_obj_t *body = ui.idlePricesBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    const PlayerPrice *changes[16];
    const size_t total = squadPriceChanges(changes, 16);
    size_t pages = 1;
    const size_t firstRow = currentRowPage(total, pages);

    bool tonight = false;
    char when[24];
    formatNextPriceUpdate(when, sizeof(when), &tonight);
    char text[40];
    if (when[0]) {
        snprintf(text, sizeof(text), "Next update %s", when);
    } else {
        strlcpy(text, "My squad", sizeof(text));
    }
    char subtitle[56];
    formatListSubtitle(subtitle, sizeof(subtitle), text, pages);
    uiScreenHeader(body, "PRICE CHANGES", subtitle);
    addCarouselDots(body);

    if (total == 0) {
        uiEmptyState(body, UI_SYM_CHECK, "No changes expected",
                     tonight ? "Your squad looks safe tonight" : "Your squad looks safe for now");
        return;
    }

    char now[32];
    char next[32];
    char detail[40];
    if (total == 1) {
        const PlayerPrice &p = *changes[0];
        const int dir = expectedPriceDirection(p);
        formatPrice(p.nowCost, now, sizeof(now));
        formatPrice(p.nowCost + dir, next, sizeof(next));
        char priceLine[80];
        snprintf(priceLine, sizeof(priceLine), "%s  " UI_SYM_ARROW "  %s", now, next);
        if (when[0]) {
            snprintf(detail, sizeof(detail), "Expected %s", when);
        } else {
            detail[0] = '\0';
        }
        const PlayerLine line = {"", uiToneColor(dir > 0 ? UiTone::Positive : UiTone::Negative),
                                 dir > 0 ? "LIKELY RISE" : "LIKELY FALL",
                                 dir > 0 ? UiTone::Positive : UiTone::Negative, detail};
        createPlayerSpotlight(body, p, kPricesKitSlot, priceLine, line);
        return;
    }

    const size_t rows = (total - firstRow) < kRowsPerPage ? (total - firstRow) : kRowsPerPage;
    const int w = rowWidthFor(rows);
    for (size_t i = 0; i < rows; ++i) {
        const PlayerPrice &p = *changes[firstRow + i];
        const int dir = expectedPriceDirection(p);
        const UiTone tone = dir > 0 ? UiTone::Positive : UiTone::Negative;
        formatPrice(p.nowCost + dir, next, sizeof(next));
        formatPrice(p.nowCost, now, sizeof(now));
        snprintf(detail, sizeof(detail), "from %s", now);
        const PlayerLine line = {next, uiToneColor(tone), dir > 0 ? UI_SYM_UP " RISE" : UI_SYM_DOWN " FALL", tone,
                                 detail};
        createPlayerRow(body, uiRowCenter(i, rows, kRowH, kRowGap, kRowBlockCy), w, p, kPricesKitSlot + i, line);
    }
}

// --- Watchlist ------------------------------------------------------------------------------------------------
static void watchStatus(const PlayerPrice &p, const char *&pill, UiTone &tone) {
    const int dir = expectedPriceDirection(p);
    if (p.locked) {
        pill = "LOCKED";
        tone = UiTone::Neutral;
    } else if (dir > 0) {
        pill = "LIKELY RISE";
        tone = UiTone::Positive;
    } else if (dir < 0) {
        pill = "LIKELY FALL";
        tone = UiTone::Negative;
    } else {
        pill = "NO CHANGE";
        tone = UiTone::Neutral;
    }
}

static void renderWatchlistPage() {
    lv_obj_t *body = ui.idleWatchBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    const PriceData &prices = idleUiData->prices;
    const size_t total = prices.watchCount;
    size_t pages = 1;
    const size_t firstRow = currentRowPage(total, pages);

    char when[24];
    formatNextPriceUpdate(when, sizeof(when));
    char text[40];
    if (when[0]) {
        snprintf(text, sizeof(text), "Next update %s", when);
    } else {
        text[0] = '\0';
    }
    char subtitle[56];
    formatListSubtitle(subtitle, sizeof(subtitle), text, pages);
    uiScreenHeader(body, "WATCHLIST", subtitle);
    addCarouselDots(body);

    char price[32];
    char detail[40];
    const char *pill = "";
    UiTone tone = UiTone::Neutral;
    if (total == 1) {
        const PlayerPrice &p = prices.watch[0];
        watchStatus(p, pill, tone);
        formatPrice(p.nowCost, price, sizeof(price));
        const int pct = (p.projectedPct < 0 ? -p.projectedPct : p.projectedPct) / 10;
        if (p.locked) {
            strlcpy(detail, "Price locked until the next update", sizeof(detail));
        } else if (pct == 0) {
            strlcpy(detail, "No transfer pressure", sizeof(detail));
        } else {
            snprintf(detail, sizeof(detail), "%d%% of the way to a %s", pct, p.projectedPct > 0 ? "rise" : "fall");
        }
        const PlayerLine line = {"", kUiTextPrimary, pill, tone, detail};
        createPlayerSpotlight(body, p, kWatchKitSlot, price, line);
        return;
    }

    const size_t rows = (total - firstRow) < kRowsPerPage ? (total - firstRow) : kRowsPerPage;
    const int w = rowWidthFor(rows);
    for (size_t i = 0; i < rows; ++i) {
        const PlayerPrice &p = prices.watch[firstRow + i];
        watchStatus(p, pill, tone);
        formatPrice(p.nowCost, price, sizeof(price));
        if (p.locked) {
            detail[0] = '\0';
        } else {
            snprintf(detail, sizeof(detail), "%+d%%", p.projectedPct / 10);
        }
        const PlayerLine line = {price, kUiTextPrimary, pill, tone, detail};
        createPlayerRow(body, uiRowCenter(i, rows, kRowH, kRowGap, kRowBlockCy), w, p, kWatchKitSlot + i, line);
    }
}

// --- Mini leagues ---------------------------------------------------------------------------------------------
//  Arsenalvision
//  #323 of 1,204          [▲ 54]
static void renderLeaguesPage() {
    lv_obj_t *body = ui.idleLeaguesBody;
    if (!body) {
        return;
    }
    lv_obj_clean(body);
    const MiniLeague *leagues[kMaxMiniLeagues];
    const size_t total = visibleLeagues(leagues, kMaxMiniLeagues);
    size_t pages = 1;
    const size_t firstRow = currentRowPage(total, pages);

    // During a gameweek FPL updates the league ranks as the scores come in, so this is the live table.
    char subtitle[24];
    formatListSubtitle(subtitle, sizeof(subtitle), inLiveCarousel() ? "Live" : "", pages);
    uiScreenHeader(body, "MINI LEAGUES", subtitle);
    addCarouselDots(body);
    if (total == 0) {
        uiEmptyState(body, UI_SYM_CHECK, "No mini leagues", "Join a league on the FPL site", UiTone::Neutral);
        return;
    }

    const size_t rows = (total - firstRow) < kRowsPerPage ? (total - firstRow) : kRowsPerPage;
    const int w = rowWidthFor(rows);
    char buf[32];
    for (size_t i = 0; i < rows; ++i) {
        const MiniLeague &lg = *leagues[firstRow + i];
        lv_obj_t *row = uiListRow(body, uiRowCenter(i, rows, kRowH, kRowGap, kRowBlockCy), w, kRowH);

        lv_obj_t *name = uiLabel(row, UI_FONT_BODY, kUiTextSecondary, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(name, w - 2 * 18);
        uiSingleLine(name);
        lv_label_set_text(name, lg.name);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 18, 9);

        lv_obj_t *line = createRowLine(row, 18, w - 18 - kRowPadX, LV_ALIGN_BOTTOM_LEFT, -7);
        lv_obj_t *rankGroup = uiBox(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rankGroup, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rankGroup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(rankGroup, 6, LV_PART_MAIN);
        lv_obj_t *rank = uiLabel(rankGroup, UI_FONT_VALUE, kUiTextPrimary, LV_TEXT_ALIGN_LEFT);
        char rankBuf[16];
        formatNumberWithCommas(lg.rank, rankBuf, sizeof(rankBuf));
        snprintf(buf, sizeof(buf), "#%s", rankBuf);
        lv_label_set_text(rank, buf);
        if (lg.size > 0) {
            lv_obj_t *of = uiLabel(rankGroup, UI_FONT_META, kUiTextSecondary, LV_TEXT_ALIGN_LEFT);
            lv_obj_set_style_pad_bottom(of, 5, LV_PART_MAIN);
            formatNumberWithCommas(lg.size, rankBuf, sizeof(rankBuf));
            snprintf(buf, sizeof(buf), "of %s", rankBuf);
            lv_label_set_text(of, buf);
        }

        // Movement since the end of the previous gameweek (positive = climbed).
        const int moved = lg.lastRank > 0 ? lg.lastRank - lg.rank : 0;
        if (moved > 0) {
            snprintf(buf, sizeof(buf), UI_SYM_UP " %d", moved);
            uiStatusPill(line, buf, UiTone::Positive);
        } else if (moved < 0) {
            snprintf(buf, sizeof(buf), UI_SYM_DOWN " %d", -moved);
            uiStatusPill(line, buf, UiTone::Negative);
        } else {
            uiStatusPill(line, UI_DASH, UiTone::Neutral);
        }
    }
}

// --- Carousel control -----------------------------------------------------------------------------------------
// `entering` = the page (or its next list screen) is just being shown, so its entrance animation plays.
static void renderIdlePage(IdlePage page, bool entering) {
    switch (page) {
        case IdlePage::Rank: renderHomeDots(); break;  // the numbers are updated continuously by updateHomeScreen
        case IdlePage::RankGraph: renderRankGraphPage(entering); break;
        case IdlePage::Prices: renderPricesPage(); break;
        case IdlePage::Watchlist: renderWatchlistPage(); break;
        case IdlePage::Leagues: renderLeaguesPage(); break;
    }
}

// Showing a page (or another screen of a list page) restarts the carousel timer, so every screen gets a full
// interval however it was reached.
static void showIdlePage(IdlePage page, bool forward = true) {
    idlePage = page;
    idlePageShownMs = millis();
    renderIdlePage(page, true);
    lv_obj_t *screen = idlePageScreen(page);
    if (currentMode == UiMode::Idle && screen && lv_screen_active() != screen) {
        lv_screen_load_anim(screen, forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT, kUiMotionMs,
                            0, false);
    }
}

// Next (or previous) screen: the list pages step through their screens of rows first. Going back lands on the
// last screen of the previous page.
static void stepIdlePage(bool forward) {
    if (forward ? carouselSubPage + 1 < idleSubPageCount(idlePage) : carouselSubPage > 0) {
        carouselSubPage = forward ? carouselSubPage + 1 : carouselSubPage - 1;
        idlePageShownMs = millis();
        renderIdlePage(idlePage, true);
        uiFadeIn(idlePageBody(idlePage));
        return;
    }
    IdlePage next = idlePage;
    for (uint8_t i = 0; i < kIdlePageCount; ++i) {
        next = static_cast<IdlePage>((static_cast<uint8_t>(next) + (forward ? 1 : kIdlePageCount - 1)) % kIdlePageCount);
        if (idlePageAvailable(next)) {
            break;
        }
    }
    carouselSubPage = forward ? 0 : idleSubPageCount(next) - 1;
    showIdlePage(next, forward);
}

static void advanceIdlePage() {
    stepIdlePage(true);
}

// --- Live carousel control ------------------------------------------------------------------------------------
static lv_obj_t *livePageList(LivePage page) {
    switch (page) {
        case LivePage::Events: return ui.eventsList;
        case LivePage::Squad: return ui.squadList;
        default: return nullptr;
    }
}

static void renderDots(lv_obj_t *container) {
    if (container) {
        lv_obj_clean(container);
        addCarouselDots(container);
    }
}

// The score and the two lists are kept up to date by updateModeUi; this redraws their dots (and, when `entering`,
// rebuilds the list so it starts from the top). The leagues and graph pages are rebuilt from idleUiData.
static void renderLivePage(LivePage page, bool entering) {
    switch (page) {
        case LivePage::Main: renderDots(ui.liveDots); break;
        case LivePage::Events:
            renderDots(ui.eventsDots);
            if (entering) {
                renderedEventsVersion = 0;
            }
            break;
        case LivePage::Squad:
            renderDots(ui.squadDots);
            if (entering) {
                renderedSquadVersion = 0;
            }
            break;
        case LivePage::Leagues: renderLeaguesPage(); break;
        case LivePage::RankGraph: renderRankGraphPage(entering); break;
    }
}

static void showLivePage(LivePage page, bool forward = true) {
    livePage = page;
    livePageShownMs = millis();
    renderLivePage(page, true);
    if (lv_obj_t *list = livePageList(page)) {
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }
    lv_obj_t *screen = livePageScreen(page);
    if (currentMode == UiMode::Live && screen && lv_screen_active() != screen) {
        lv_screen_load_anim(screen, forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT, kUiMotionMs,
                            0, false);
    }
}

static size_t liveSubPageCount(LivePage page) {
    return page == LivePage::Leagues ? idleSubPageCount(IdlePage::Leagues) : 1;
}

// As stepIdlePage. (The events and squad lists scroll rather than page.)
static void stepLivePage(bool forward) {
    if (forward ? carouselSubPage + 1 < liveSubPageCount(livePage) : carouselSubPage > 0) {
        carouselSubPage = forward ? carouselSubPage + 1 : carouselSubPage - 1;
        livePageShownMs = millis();
        renderLivePage(livePage, true);
        uiFadeIn(ui.idleLeaguesBody);
        return;
    }
    LivePage next = livePage;
    for (uint8_t i = 0; i < kLivePageCount; ++i) {
        next = static_cast<LivePage>((static_cast<uint8_t>(next) + (forward ? 1 : kLivePageCount - 1)) % kLivePageCount);
        if (livePageAvailable(next)) {
            break;
        }
    }
    carouselSubPage = forward ? 0 : liveSubPageCount(next) - 1;
    showLivePage(next, forward);
}

static void advanceLivePage() {
    stepLivePage(true);
}

// Tapping a carousel page (anywhere but a button): the left third goes back a screen, the rest goes forward.
static void carouselTapEventCb(lv_event_t *) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !isTap()) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const bool forward = p.x >= kUiScreenSize / 3;
    if (currentMode == UiMode::Idle) {
        stepIdlePage(forward);
    } else if (currentMode == UiMode::Live) {
        stepLivePage(forward);
    }
}

static void serviceLiveCarousel(bool dataChanged) {
    if (!livePageAvailable(livePage)) {
        advanceLivePage();
        return;
    }
    if (dataChanged) {
        renderLivePage(livePage, false);  // also refreshes the dots when a page came or went
    }
    const uint32_t intervalMs = static_cast<uint32_t>(idleUiSettings.screenIntervalSec) * 1000U;
    if (demoCarouselPaused || touchPressStartMs != 0) {
        livePageShownMs = millis();  // scrolling a list, or resuming, gives the page a full interval
    } else if (intervalMs > 0 && millis() - livePageShownMs >= intervalMs) {
        // A list scrolls down a screenful at a time before the carousel moves on.
        lv_obj_t *list = livePageList(livePage);
        if (list && lv_obj_get_scroll_bottom(list) > 0) {
            lv_obj_scroll_by_bounded(list, 0, -(lv_obj_get_height(list) - 48), LV_ANIM_ON);
            livePageShownMs = millis();
        } else {
            advanceLivePage();
        }
    }
}

// Called from uiTask every loop: picks up new data/settings and advances the carousels on their timers.
static void serviceCarousels() {
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

    if (currentMode == UiMode::Live) {
        serviceLiveCarousel(changed);
        return;
    }
    if (currentMode != UiMode::Idle) {
        return;
    }
    if (!idlePageAvailable(idlePage)) {
        advanceIdlePage();
        return;
    }
    if (changed) {
        renderIdlePage(idlePage, false);
    }
    const uint32_t intervalMs = static_cast<uint32_t>(idleUiSettings.screenIntervalSec) * 1000U;
    if (demoCarouselPaused || touchPressStartMs != 0) {
        idlePageShownMs = millis();  // resuming, or letting go, gives the current page a full interval
    } else if (intervalMs > 0 && millis() - idlePageShownMs >= intervalMs) {
        advanceIdlePage();
    }
}

// Serves the `fps` serial command: advances the carousel, then reports the frames drawn in the next 500 ms.
static void serviceFpsProbe() {
    if (fpsRequested && fpsStartMs == 0) {
        fpsRequested = false;
        fpsFrames = 0;
        fpsLongestMs = 0;
        fpsFlushUs = 0;
        fpsFlushes = 0;
        fpsDmaUs = 0;
        fpsDmaBytes = 0;
        fpsStartMs = millis();
        if (currentMode == UiMode::Idle) {
            advanceIdlePage();
        }
        return;
    }
    if (fpsStartMs != 0 && millis() - fpsStartMs >= 500) {
        Serial.printf("[FPS] %u frames in 500 ms (%u fps), longest frame %u ms, %u flushes, %u ms in flush\n",
                      static_cast<unsigned>(fpsFrames), static_cast<unsigned>(fpsFrames * 2),
                      static_cast<unsigned>(fpsLongestMs), static_cast<unsigned>(fpsFlushes),
                      static_cast<unsigned>(fpsFlushUs / 1000));
        Serial.printf("[FPS] panel transfers: %u KB in %u ms\n", static_cast<unsigned>(fpsDmaBytes / 1024),
                      static_cast<unsigned>(fpsDmaUs / 1000));
        fpsStartMs = 0;
        // Cost of one full-screen redraw of the page now showing, averaged over a few frames.
        const uint32_t startUs = micros();
        for (int i = 0; i < 5; ++i) {
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(nullptr);
        }
        Serial.printf("[FPS] full redraw of this page: %u ms\n", static_cast<unsigned>((micros() - startUs) / 5000));
        // Same for an empty screen: the floor set by filling and sending the frame.
        lv_obj_t *previous = lv_screen_active();
        lv_obj_t *blank = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(blank, lv_color_hex(kUiBg), LV_PART_MAIN);
        lv_screen_load(blank);
        const uint32_t blankStartUs = micros();
        for (int i = 0; i < 5; ++i) {
            lv_obj_invalidate(blank);
            lv_refr_now(nullptr);
        }
        Serial.printf("[FPS] full redraw of an empty screen: %u ms\n",
                      static_cast<unsigned>((micros() - blankStartUs) / 5000));
        lv_screen_load(previous);
        lv_obj_delete(blank);
    }
}

// Serves the `screenshot [page]` serial command (see screenshotFrame).
static void serviceScreenshot() {
    if (!screenshotRequested) {
        return;
    }
    if (screenshotAtMs == 0) {
        const int page = screenshotPage;
        if (page >= 0 && page < kIdlePageCount && currentMode == UiMode::Idle) {
            carouselSubPage = 0;
            showIdlePage(static_cast<IdlePage>(page));
        } else if (page >= kIdlePageCount && page <= kIdlePageCount + 2 && currentMode == UiMode::Live) {
            carouselSubPage = 0;
            showLivePage(page == kIdlePageCount ? LivePage::Events : LivePage::Squad);
        }
        screenshotAtMs = millis() + (page >= 0 ? 1500U : 0U);  // let entrance animations finish
        return;
    }
    if (static_cast<int32_t>(millis() - screenshotAtMs) < 0) {
        return;
    }
    screenshotRequested = false;
    screenshotAtMs = 0;
    if (!screenshotFrame) {
        screenshotFrame = static_cast<uint16_t *>(heap_caps_malloc(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 2,
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!screenshotFrame) {
            Serial.println("SS ERROR no memory");
            return;
        }
    }
    if (screenshotPage == kIdlePageCount + 2 && ui.squadList) {
        lv_obj_scroll_to_y(ui.squadList, LV_COORD_MAX, LV_ANIM_OFF);
    }
    screenshotCapturing = true;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
    screenshotCapturing = false;

    static char hex[kDisplayWidth * 4 + 1];
    static const char kDigits[] = "0123456789abcdef";
    Serial.printf("SS BEGIN %u %u\n", static_cast<unsigned>(kDisplayWidth), static_cast<unsigned>(kDisplayHeight));
    for (int y = 0; y < kDisplayHeight; ++y) {
        const uint16_t *row = screenshotFrame + y * kDisplayWidth;
        for (int x = 0; x < kDisplayWidth; ++x) {
            const uint16_t v = row[x];
            hex[x * 4 + 0] = kDigits[(v >> 12) & 0xF];
            hex[x * 4 + 1] = kDigits[(v >> 8) & 0xF];
            hex[x * 4 + 2] = kDigits[(v >> 4) & 0xF];
            hex[x * 4 + 3] = kDigits[v & 0xF];
        }
        hex[kDisplayWidth * 4] = '\0';
        Serial.printf("SS %d %s\n", y, hex);
    }
    Serial.println("SS END");
}

static void showEventsEventCb(lv_event_t *) {
    if (currentMode == UiMode::Live && isTap()) {
        carouselSubPage = 0;
        showLivePage(LivePage::Events);
    }
}

// Clears the events list (and the live card), so from now on it only shows what happens next.
static void clearEventsCb(lv_event_t *) {
    if (isTap()) {
        clearUiEvents();
        tickerEventIndex = 0;
    }
}

// ---------------------------------------------------------------------------
// Gameweek screens: deadline countdown, final hour, live gameweek, point pop-ups, and the events and squad lists.
// Same design language as the carousel (ui_kit). The widgets are built once in createUi and updated every UI
// loop by updateGameweekScreens through uiSetText / uiNumberSet, which only redraw what changed.
// ---------------------------------------------------------------------------
static constexpr int kListTop = 80;
static constexpr int kListW = 300;
static constexpr int kEventsListH = 234;  // leaves room for the Clear button above the page dots
static constexpr int kSquadListH = 262;
static constexpr int kListRowW = kListW - 4;
static constexpr int kClearButtonY = 324;
static int liveCardW = 0;  // width of the live screen's event card, set when it is built

// "CLEAN SHEET!" -> "CLEAN SHEET" (upper) or "Clean sheet" (sentence case).
static void formatEventLabel(const char *label, bool upper, char *out, size_t outLen) {
    size_t n = 0;
    for (size_t i = 0; label && label[i] && n + 1 < outLen; ++i) {
        const char c = label[i];
        if (c == '!') {
            continue;
        }
        out[n] = static_cast<char>(upper || n == 0 ? toupper(static_cast<unsigned char>(c))
                                                   : tolower(static_cast<unsigned char>(c)));
        ++n;
    }
    while (n > 0 && out[n - 1] == ' ') {
        --n;
    }
    out[n] = '\0';
    if (n == 0) {
        strlcpy(out, upper ? "POINTS" : "Points", outLen);
    }
}

static UiTone eventTone(const UiEventItem &e) {
    if (strcmp(e.icon, "YC") == 0) {
        return UiTone::Warning;
    }
    return e.delta < 0 ? UiTone::Negative : UiTone::Positive;
}

static void formatAge(uint32_t sinceMs, char *out, size_t outLen) {
    const uint32_t minutes = (millis() - sinceMs) / 60000U;
    if (minutes < 1) {
        strlcpy(out, "now", outLen);
    } else if (minutes < 60) {
        snprintf(out, outLen, "%um ago", static_cast<unsigned>(minutes));
    } else {
        snprintf(out, outLen, "%uh ago", static_cast<unsigned>(minutes / 60));
    }
}

// Round badge with an event's short code ("G", "A", "CS", ...), tinted by its tone.
static lv_obj_t *createEventBadge(lv_obj_t *parent) {
    lv_obj_t *badge = uiStatusPill(parent, "", UiTone::Neutral);
    lv_obj_set_size(badge, 36, 36);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_center(lv_obj_get_child(badge, 0));
    return badge;
}

// Recent events rolled up per player, so "Haaland +1" then "Haaland +4" reads as one "Haaland +5".
struct PlayerEvents {
    char player[24] = "";
    char team[24] = "";
    char icon[8] = "";  // code of the player's biggest single event, for the badge
    int biggest = 0;    // |points| of that event
    int total = 0;
    uint32_t latestMs = 0;
    static constexpr size_t kMaxLabels = 8;
    char labels[kMaxLabels][20] = {};  // what happened, in match order ("Goal", "Assist"), counted in labelCounts
    uint8_t labelCounts[kMaxLabels] = {};
    int labelPoints[kMaxLabels] = {};  // points from each
    size_t labelCount = 0;
};

// Groups the recent events by player, the player with the newest event first. Returns the number of players.
static size_t groupEventsByPlayer(const UiRuntimeState &runtime, PlayerEvents *out, size_t capacity) {
    size_t n = 0;
    for (size_t i = 0; i < runtime.recentEventCount; ++i) {  // oldest first, so labels read in match order
        const UiEventItem &e = runtime.recentEvents[i];
        PlayerEvents *p = nullptr;
        for (size_t j = 0; j < n; ++j) {
            if (strcmp(out[j].player, e.player) == 0 && strcmp(out[j].team, e.team) == 0) {
                p = &out[j];
                break;
            }
        }
        if (!p) {
            if (n >= capacity) {
                continue;
            }
            p = &out[n++];
            *p = PlayerEvents{};
            strlcpy(p->player, e.player, sizeof(p->player));
            strlcpy(p->team, e.team, sizeof(p->team));
        }
        p->total += e.delta;
        p->latestMs = e.epochMs;
        const int size = e.delta < 0 ? -e.delta : e.delta;
        if (!p->icon[0] || size >= p->biggest) {
            strlcpy(p->icon, e.icon[0] ? e.icon : (e.delta < 0 ? "-" : "+"), sizeof(p->icon));
            p->biggest = size;
        }
        char label[20];
        formatEventLabel(e.label, false, label, sizeof(label));
        size_t k = 0;
        while (k < p->labelCount && strcmp(p->labels[k], label) != 0) {
            ++k;
        }
        if (k < p->labelCount) {
            ++p->labelCounts[k];
            p->labelPoints[k] += e.delta;
        } else if (p->labelCount < PlayerEvents::kMaxLabels) {
            strlcpy(p->labels[p->labelCount], label, sizeof(p->labels[0]));
            p->labelCounts[p->labelCount] = 1;
            p->labelPoints[p->labelCount++] = e.delta;
        }
    }
    for (size_t i = 1; i < n; ++i) {  // newest activity first
        for (size_t j = i; j > 0 && out[j].latestMs > out[j - 1].latestMs; --j) {
            const PlayerEvents tmp = out[j - 1];
            out[j - 1] = out[j];
            out[j] = tmp;
        }
    }
    return n;
}

// "Goal x2 +8, Assist +3, Bonus pts +2"
static void formatPlayerEventLabels(const PlayerEvents &p, char *out, size_t outLen) {
    out[0] = '\0';
    for (size_t i = 0; i < p.labelCount; ++i) {
        char part[40];
        if (p.labelCounts[i] > 1) {
            snprintf(part, sizeof(part), "%s%s x%u %+d", i ? ", " : "", p.labels[i],
                     static_cast<unsigned>(p.labelCounts[i]), p.labelPoints[i]);
        } else {
            snprintf(part, sizeof(part), "%s%s %+d", i ? ", " : "", p.labels[i], p.labelPoints[i]);
        }
        strlcat(out, part, outLen);
    }
}

// A player's net points: gains are Positive, losses Negative (amber when it's just a booking), zero Neutral.
static UiTone playerEventsTone(const PlayerEvents &p) {
    if (p.total < 0) {
        return strcmp(p.icon, "YC") == 0 ? UiTone::Warning : UiTone::Negative;
    }
    return p.total > 0 ? UiTone::Positive : UiTone::Neutral;
}

static void setPlayerEventsBadge(lv_obj_t *badge, const PlayerEvents &p) {
    uiStatusPillSet(badge, p.icon, playerEventsTone(p));
}

// Pill-shaped button; `font` sets its size.
static lv_obj_t *createPillButton(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_event_cb_t cb) {
    lv_obj_t *button = uiStatusPill(parent, text, UiTone::Neutral, font);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, lv_color_hex(kUiSurface), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(kUiSurfaceRaised), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_hex(kUiTextPrimary), LV_PART_MAIN);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);
    return button;
}

// A live carousel page with a heading and a scrolling column of rows `h` high. Tapping it turns the page; the
// page dots go in dotsOut. Returns the column.
static lv_obj_t *createListScreen(lv_obj_t *screen, const char *title, int h, lv_obj_t *&dotsOut) {
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, carouselTapEventCb, LV_EVENT_CLICKED, nullptr);
    uiScreenHeader(screen, title);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, kListW, h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, kListTop);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, carouselTapEventCb, LV_EVENT_CLICKED, nullptr);  // LVGL sends no click after a scroll

    dotsOut = uiBox(screen, kUiScreenSize, kUiScreenSize);
    lv_obj_set_pos(dotsOut, 0, 0);
    return list;
}

static lv_obj_t *createListRow(lv_obj_t *list, int h) {
    lv_obj_t *row = uiBox(list, kListRowW, h);
    lv_obj_set_style_bg_color(row, lv_color_hex(kUiSurface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    return row;
}

//  (G)  Haaland                 +9
//       Playing +1, 60+ mins +1,
//       Goal +4, Assist +3 · 12m ago
// The reasons wrap onto as many lines as they need.
static void refreshEventsList(const UiRuntimeState &runtime) {
    if (!ui.eventsList) {
        return;
    }
    lv_obj_clean(ui.eventsList);
    static PlayerEvents players[kMaxUiEvents];  // UI task only
    const size_t count = groupEventsByPlayer(runtime, players, kMaxUiEvents);
    if (ui.eventsClearBtn) {
        if (count == 0) {
            lv_obj_add_flag(ui.eventsClearBtn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(ui.eventsClearBtn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (count == 0) {
        lv_obj_t *empty = uiLabel(ui.eventsList, UI_FONT_BODY, kUiTextSecondary);
        lv_obj_set_style_pad_top(empty, 96, LV_PART_MAIN);
        lv_label_set_text(empty, "No new points events");
        return;
    }

    static constexpr int kTextX = 60;
    static constexpr int kTextW = kListRowW - kTextX - 16;
    static constexpr int kDeltaW = 64;
    char buf[240];
    char labels[200];
    char age[16];
    for (size_t i = 0; i < count; ++i) {
        const PlayerEvents &p = players[i];
        lv_obj_t *row = createListRow(ui.eventsList, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_bottom(row, 10, LV_PART_MAIN);
        lv_obj_t *badge = createEventBadge(row);
        setPlayerEventsBadge(badge, p);
        lv_obj_set_pos(badge, 12, 10);

        lv_obj_t *name = uiLabel(row, UI_FONT_NAME, kUiTextPrimary, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(name, kTextW - kDeltaW);
        uiSingleLine(name);
        lv_label_set_text(name, p.player);
        lv_obj_set_pos(name, kTextX, 8);

        snprintf(buf, sizeof(buf), "%+d", p.total);
        lv_obj_t *delta = uiLabel(row, UI_FONT_VALUE, uiToneColor(playerEventsTone(p)), LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(delta, kDeltaW);
        lv_label_set_text(delta, buf);
        lv_obj_set_pos(delta, kListRowW - 16 - kDeltaW, 2);

        formatPlayerEventLabels(p, labels, sizeof(labels));
        formatAge(p.latestMs, age, sizeof(age));
        snprintf(buf, sizeof(buf), "%s " UI_DOT " %s", labels, age);
        lv_obj_t *meta = uiLabel(row, UI_FONT_META, kUiTextSecondary, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(meta, kTextW);
        lv_label_set_long_mode(meta, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(meta, 2, LV_PART_MAIN);
        lv_label_set_text(meta, buf);
        lv_obj_set_pos(meta, kTextX, 38);
    }
}

//  Saka              [C]  G +5    12
static void refreshSquadList(const UiRuntimeState &runtime) {
    if (!ui.squadList) {
        return;
    }
    lv_obj_clean(ui.squadList);
    bool benchStarted = false;
    char buf[16];
    for (size_t i = 0; i < runtime.squadCount; ++i) {
        const UiSquadRow &r = runtime.squadRows[i];
        if (r.isBench && !benchStarted) {
            benchStarted = true;
            lv_obj_t *section = uiLabel(ui.squadList, UI_FONT_META, kUiTextSecondary, LV_TEXT_ALIGN_LEFT);
            lv_obj_set_width(section, kListRowW - 24);
            lv_obj_set_style_text_letter_space(section, 2, LV_PART_MAIN);
            lv_obj_set_style_pad_top(section, 6, LV_PART_MAIN);
            lv_label_set_text(section, "BENCH");
        }
        lv_obj_t *row = createListRow(ui.squadList, 46);
        if (r.isBench) {
            lv_obj_set_style_bg_opa(row, LV_OPA_50, LV_PART_MAIN);
        }
        lv_obj_t *line = createRowLine(row, 16, kListRowW - 16 - kRowPadX, LV_ALIGN_LEFT_MID, 0);
        lv_obj_t *name = createGrowingName(line, UI_FONT_BODY, r.player);
        if (r.isBench) {
            lv_obj_set_style_text_color(name, lv_color_hex(kUiTextSecondary), LV_PART_MAIN);
        }
        if (r.isCaptain || r.isViceCaptain) {
            uiStatusPill(line, r.isCaptain ? "C" : "V", r.isCaptain ? UiTone::Accent : UiTone::Neutral);
        }
        if (r.breakdown[0]) {
            lv_obj_t *breakdown = uiLabel(line, UI_FONT_META, kUiTextSecondary);
            lv_label_set_text(breakdown, r.breakdown);
        }
        lv_obj_t *points = uiLabel(line, UI_FONT_NAME, r.hasPlayed ? kUiTextPrimary : kUiTextSecondary,
                                   LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_style_min_width(points, 30, LV_PART_MAIN);
        if (r.hasPlayed) {
            snprintf(buf, sizeof(buf), "%d", r.points);
        } else {
            strlcpy(buf, UI_DASH, sizeof(buf));
        }
        lv_label_set_text(points, buf);
    }

    // Scripted demos (pop-up / deadline countdowns), for showing the device off.
    lv_obj_t *demo = uiBox(ui.squadList, kListRowW, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(demo, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(demo, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(demo, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(demo, 8, LV_PART_MAIN);
    lv_obj_t *buttons[3] = {createPillButton(demo, "DEMO POP-UP", UI_FONT_PILL, squadDemoNotificationBtnCb),
                            createPillButton(demo, "45M", UI_FONT_PILL, squadDemoDeadline45BtnCb),
                            createPillButton(demo, "7M", UI_FONT_PILL, squadDemoDeadline7BtnCb)};
    for (lv_obj_t *button : buttons) {
        lv_obj_set_style_pad_ver(button, 9, LV_PART_MAIN);
    }
}

// --- Deadline and final hour ----------------------------------------------------------------------------------
static void createDeadlineScreens() {
    lv_obj_t *s = ui.screenDeadline;
    uiScreenHeader(s, "DEADLINE");
    ui.deadlineMeta = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_align(ui.deadlineMeta, LV_ALIGN_TOP_MID, 0, kUiHeaderY + 26);
    ui.deadlineCountdown = uiLabel(s, UI_FONT_HERO, kUiTextPrimary);
    lv_label_set_text(ui.deadlineCountdown, "--:--:--");
    lv_obj_align(ui.deadlineCountdown, LV_ALIGN_CENTER, 0, -20);
    ui.deadlineLabel = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_set_style_text_letter_space(ui.deadlineLabel, 2, LV_PART_MAIN);
    lv_label_set_text(ui.deadlineLabel, "HRS     MIN     SEC");
    lv_obj_align(ui.deadlineLabel, LV_ALIGN_CENTER, 0, 20);
    ui.deadlineWhen = uiStatusPill(s, "", UiTone::Neutral, UI_FONT_BODY);
    lv_obj_align(ui.deadlineWhen, LV_ALIGN_CENTER, 0, 80);

    s = ui.screenFinalHour;
    ui.finalArc = lv_arc_create(s);
    lv_obj_set_size(ui.finalArc, kUiScreenSize, kUiScreenSize);
    lv_obj_center(ui.finalArc);
    lv_arc_set_rotation(ui.finalArc, 270);
    lv_arc_set_bg_angles(ui.finalArc, 0, 360);
    lv_arc_set_range(ui.finalArc, 0, 3600);
    lv_arc_set_value(ui.finalArc, 3600);
    lv_obj_set_style_arc_width(ui.finalArc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.finalArc, lv_color_hex(kUiSurface), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.finalArc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.finalArc, lv_color_hex(kUiWarning), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.finalArc, true, LV_PART_INDICATOR);
    lv_obj_remove_style(ui.finalArc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(ui.finalArc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *heading = uiLabel(s, UI_FONT_HEADING, kUiWarning);
    lv_obj_set_style_text_letter_space(heading, 2, LV_PART_MAIN);
    lv_label_set_text(heading, "FINAL HOUR");
    lv_obj_align(heading, LV_ALIGN_CENTER, 0, -72);
    ui.finalCountdown = uiLabel(s, UI_FONT_HERO, kUiTextPrimary);
    lv_label_set_text(ui.finalCountdown, "--:--");
    lv_obj_align(ui.finalCountdown, LV_ALIGN_CENTER, 0, -10);
    ui.finalMeta = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_align(ui.finalMeta, LV_ALIGN_CENTER, 0, 48);
}

// --- Live -----------------------------------------------------------------------------------------------------
//        ● GW6 LIVE
//            64
//          POINTS
//         LIVE RANK
//    538,544  [▲ 12,345]
//   [ (G) Saka      +6  > ]
static void createLiveScreen() {
    lv_obj_t *s = ui.screenLive;
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = uiBox(s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title, 10, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, kUiHeaderY);
    ui.liveDot = uiDot(title, 10, kUiAccent);
    uiBreathe(ui.liveDot);
    ui.liveTitle = uiLabel(title, UI_FONT_HEADING, kUiAccent);
    lv_obj_set_style_text_letter_space(ui.liveTitle, 2, LV_PART_MAIN);
    lv_label_set_text(ui.liveTitle, "LIVE");

    ui.livePoints = uiLabel(s, UI_FONT_HERO, kUiTextPrimary);
    uiNumberInit(ui.livePoints);
    lv_label_set_text(ui.livePoints, "--");
    lv_obj_align(ui.livePoints, LV_ALIGN_CENTER, 0, -80);
    lv_obj_t *pointsLabel = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_set_style_text_letter_space(pointsLabel, 2, LV_PART_MAIN);
    lv_label_set_text(pointsLabel, "GW POINTS");
    lv_obj_align(pointsLabel, LV_ALIGN_CENTER, 0, -36);

    lv_obj_t *rankLabel = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_set_style_text_letter_space(rankLabel, 2, LV_PART_MAIN);
    lv_label_set_text(rankLabel, "LIVE RANK");
    lv_obj_align(rankLabel, LV_ALIGN_CENTER, 0, 4);
    lv_obj_t *rankRow = uiBox(s, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rankRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rankRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rankRow, 10, LV_PART_MAIN);
    lv_obj_align(rankRow, LV_ALIGN_CENTER, 0, 38);
    ui.liveRank = uiLabel(rankRow, UI_FONT_VALUE, kUiTextPrimary);
    uiNumberInit(ui.liveRank);
    lv_label_set_text(ui.liveRank, "--");
    ui.liveRankMove = uiStatusPill(rankRow, "", UiTone::Neutral);
    lv_obj_add_flag(ui.liveRankMove, LV_OBJ_FLAG_HIDDEN);

    // Latest points event; tap it for the events page (a tap anywhere else turns to the next page).
    static constexpr int kCardCy = 114;
    static constexpr int kCardH = 60;
    const int cardW = uiSafeWidth(kCardCy, kCardH - 12, 8);
    liveCardW = cardW;
    ui.liveTickerBtn = uiListRow(s, kCardCy, cardW, kCardH);
    lv_obj_add_flag(ui.liveTickerBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ui.liveTickerBtn, lv_color_hex(kUiSurfaceRaised), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui.liveTickerBtn, showEventsEventCb, LV_EVENT_CLICKED, nullptr);
    ui.liveEventBadge = createEventBadge(ui.liveTickerBtn);
    lv_obj_align(ui.liveEventBadge, LV_ALIGN_LEFT_MID, 14, 0);
    ui.liveTickerLabel = uiLabel(ui.liveTickerBtn, UI_FONT_NAME, kUiTextPrimary, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(ui.liveTickerLabel, cardW - 62 - 90);
    uiSingleLine(ui.liveTickerLabel);
    lv_obj_align(ui.liveTickerLabel, LV_ALIGN_LEFT_MID, 62, 0);
    ui.liveEventDelta = uiLabel(ui.liveTickerBtn, UI_FONT_NAME, kUiPositive, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(ui.liveEventDelta, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_t *chevron = uiLabel(ui.liveTickerBtn, UI_FONT_META, kUiTextSecondary);
    lv_label_set_text(chevron, UI_SYM_ARROW);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_add_event_cb(s, carouselTapEventCb, LV_EVENT_CLICKED, nullptr);
    ui.liveDots = uiBox(s, kUiScreenSize, kUiScreenSize);
    lv_obj_set_pos(ui.liveDots, 0, 0);
}

// --- Points pop-up --------------------------------------------------------------------------------------------
//          GOAL
//         [kit]
//          Saka
//        [+6 PTS]
//      51  ->  57 pts
static void createPopupScreen() {
    lv_obj_t *s = ui.screenPopup;
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    ui.popupTitle = uiLabel(s, UI_FONT_VALUE, kUiPositive);
    lv_obj_set_style_text_letter_space(ui.popupTitle, 2, LV_PART_MAIN);
    lv_obj_set_width(ui.popupTitle, uiSafeWidth(-140, 36));
    uiSingleLine(ui.popupTitle);
    lv_obj_align(ui.popupTitle, LV_ALIGN_CENTER, 0, -140);

    memset(&kitImageDsc, 0, sizeof(kitImageDsc));
    kitImageDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    kitImageDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    kitImageDsc.header.w = kKitWidth;
    kitImageDsc.header.h = kKitHeight;
    kitImageDsc.data_size = kKitRgb565Bytes;
    kitImageDsc.data = kitImageBuffer;
    ui.popupKit = lv_image_create(s);
    lv_image_set_src(ui.popupKit, &kitImageDsc);
    lv_obj_align(ui.popupKit, LV_ALIGN_CENTER, 0, -42);

    ui.popupPlayer = uiLabel(s, UI_FONT_VALUE, kUiTextPrimary);
    lv_obj_set_width(ui.popupPlayer, uiSafeWidth(66, 36));
    uiSingleLine(ui.popupPlayer);
    lv_obj_align(ui.popupPlayer, LV_ALIGN_CENTER, 0, 66);
    ui.popupDelta = uiStatusPill(s, "", UiTone::Positive, UI_FONT_NAME);
    lv_obj_align(ui.popupDelta, LV_ALIGN_CENTER, 0, 106);
    ui.popupTotal = uiLabel(s, UI_FONT_META, kUiTextSecondary);
    lv_obj_align(ui.popupTotal, LV_ALIGN_CENTER, 0, 144);
}

static void fillPopup(const UiEventItem &event) {
    const UiTone tone = eventTone(event);
    const uint32_t color = uiToneColor(tone);
    char buf[48];
    formatEventLabel(event.label, true, buf, sizeof(buf));
    uiSetText(ui.popupTitle, buf);
    uiSetTextColor(ui.popupTitle, color);
    uiSetText(ui.popupPlayer, event.player);
    snprintf(buf, sizeof(buf), "%+d PTS", event.delta);
    uiStatusPillSet(ui.popupDelta, buf, tone);
    snprintf(buf, sizeof(buf), "%d  " UI_SYM_ARROW "  %d pts", event.totalBefore, event.totalAfter);
    uiSetText(ui.popupTotal, buf);

    if (resolveAndLoadKitImage(event)) {
        lv_obj_remove_flag(ui.popupKit, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(ui.popupKit, &kitImageDsc);
        uiPopIn(ui.popupKit, 120);
    } else {
        lv_obj_add_flag(ui.popupKit, LV_OBJ_FLAG_HIDDEN);
    }
    uiFadeIn(ui.popupDelta, 250, 260);
    uiPulseRing(color);
}

// --- Updates --------------------------------------------------------------------------------------------------
static int64_t secondsUntilDeadline(const SharedUiState &state) {
    const time_t now = time(nullptr);
    const int64_t sec = now > 100000 ? static_cast<int64_t>(state.nextDeadlineUtc) - static_cast<int64_t>(now) : 0;
    return sec > 0 ? sec : 0;
}

static void updateGameweekScreens(const SharedUiState &state, const UiRuntimeState &runtime) {
    char buf[64];

    if (state.hasNextDeadline) {
        const int64_t sec = secondsUntilDeadline(state);
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", static_cast<int>(sec / 3600), static_cast<int>((sec % 3600) / 60),
                 static_cast<int>(sec % 60));
        uiSetText(ui.deadlineCountdown, buf);
        snprintf(buf, sizeof(buf), "Gameweek %d", state.hasNextGw ? state.nextGw : 0);
        uiSetText(ui.deadlineMeta, buf);

        const time_t deadline = state.nextDeadlineUtc;
        struct tm local;
        localtime_r(&deadline, &local);
        static const char *const kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        snprintf(buf, sizeof(buf), "%s %02d:%02d", kDays[local.tm_wday % 7], local.tm_hour, local.tm_min);
        uiStatusPillSet(ui.deadlineWhen, buf, UiTone::Neutral);

        const int64_t finalSec = sec > 3600 ? 3600 : sec;
        snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>(finalSec / 60), static_cast<int>(finalSec % 60));
        uiSetText(ui.finalCountdown, buf);
        snprintf(buf, sizeof(buf), "GW%d deadline %02d:%02d", state.hasNextGw ? state.nextGw : 0, local.tm_hour,
                 local.tm_min);
        uiSetText(ui.finalMeta, buf);
        lv_arc_set_value(ui.finalArc, static_cast<int32_t>(finalSec));
        // The ring turns from amber to red for the last five minutes.
        static bool finalUrgent = false;
        const bool urgent = finalSec < 300;
        if (urgent != finalUrgent) {
            lv_obj_set_style_arc_color(ui.finalArc, lv_color_hex(urgent ? kUiNegative : kUiWarning), LV_PART_INDICATOR);
            finalUrgent = urgent;
        }
    }

    snprintf(buf, sizeof(buf), "GW%d LIVE", state.currentGw);
    uiSetText(ui.liveTitle, buf);
    static int dotStale = -1;
    if (dotStale != (state.isStale ? 1 : 0)) {
        dotStale = state.isStale ? 1 : 0;
        lv_obj_set_style_bg_color(ui.liveDot, lv_color_hex(state.isStale ? kUiWarning : kUiAccent), LV_PART_MAIN);
        if (state.isStale) {
            uiBreatheStop(ui.liveDot);
        } else {
            uiBreathe(ui.liveDot);
        }
    }
    if (state.hasGwPoints) {
        uiNumberSet(ui.livePoints, state.gwPoints, true);
    } else {
        uiNumberClear(ui.livePoints, "--");
    }
    if (state.hasRankData) {
        uiNumberSet(ui.liveRank, state.overallRank, true);
        char diffBuf[24];
        formatNumberWithCommas(state.rankDiff < 0 ? -state.rankDiff : state.rankDiff, diffBuf, sizeof(diffBuf));
        if (state.rankDiff != 0) {
            snprintf(buf, sizeof(buf), "%s %s", state.rankDiff > 0 ? UI_SYM_UP : UI_SYM_DOWN, diffBuf);
            uiStatusPillSet(ui.liveRankMove, buf, state.rankDiff > 0 ? UiTone::Positive : UiTone::Negative);
            lv_obj_remove_flag(ui.liveRankMove, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.liveRankMove, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        uiNumberClear(ui.liveRank, "--");
        lv_obj_add_flag(ui.liveRankMove, LV_OBJ_FLAG_HIDDEN);
    }

    // The event card cycles through the players with recent events (points summed per player), newest first.
    // With none, its text takes the whole card.
    static PlayerEvents cardPlayers[kMaxUiEvents];  // regrouped only when the events change
    static size_t cardPlayerCount = 0;
    static uint32_t cardEventVersion = UINT32_MAX;
    if (cardEventVersion != runtime.eventVersion) {
        cardEventVersion = runtime.eventVersion;
        cardPlayerCount = groupEventsByPlayer(runtime, cardPlayers, kMaxUiEvents);
    }
    static int cardEmpty = -1;
    if (cardEmpty != (runtime.recentEventCount == 0 ? 1 : 0)) {
        cardEmpty = runtime.recentEventCount == 0 ? 1 : 0;
        lv_obj_set_width(ui.liveTickerLabel, cardEmpty ? liveCardW - 20 - 40 : liveCardW - 62 - 90);
        lv_obj_align(ui.liveTickerLabel, LV_ALIGN_LEFT_MID, cardEmpty ? 20 : 62, 0);
    }
    if (runtime.recentEventCount == 0) {
        uiSetText(ui.liveTickerLabel, "No events yet");
        uiSetTextColor(ui.liveTickerLabel, kUiTextSecondary);
        lv_obj_add_flag(ui.liveEventBadge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.liveEventDelta, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (millis() - lastTickerRotateMs > 3000U) {
            tickerEventIndex = cardPlayerCount ? (tickerEventIndex + 1) % cardPlayerCount : 0;
            lastTickerRotateMs = millis();
        }
        if (tickerEventIndex >= cardPlayerCount) {
            tickerEventIndex = 0;
        }
        const PlayerEvents &p = cardPlayers[tickerEventIndex];
        uiSetText(ui.liveTickerLabel, p.player);
        uiSetTextColor(ui.liveTickerLabel, kUiTextPrimary);
        setPlayerEventsBadge(ui.liveEventBadge, p);
        snprintf(buf, sizeof(buf), "%+d", p.total);
        uiSetText(ui.liveEventDelta, buf);
        uiSetTextColor(ui.liveEventDelta, uiToneColor(playerEventsTone(p)));
        lv_obj_remove_flag(ui.liveEventBadge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui.liveEventDelta, LV_OBJ_FLAG_HIDDEN);
    }
}

// Battery: sampled on the UI task and smoothed, since WiFi bursts and backlight changes pull the voltage around.
static constexpr uint32_t kBatterySampleMs = 5000;
static float batterySmoothedVolts = 0.0f;
static volatile int batteryTestPercent = -1;  // `device battery <percent>` shows this instead, -1 = measured

static void batterySampleCb(lv_timer_t *) {
    static constexpr float kSmoothing = 0.2f;
    const float volts = batteryVolts();
    batterySmoothedVolts =
        batterySmoothedVolts <= 0.0f ? volts : batterySmoothedVolts + kSmoothing * (volts - batterySmoothedVolts);
    const int testPercent = batteryTestPercent;
    uiBatterySet(testPercent >= 0 ? testPercent : batteryPercent(batterySmoothedVolts));
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

    styleScreen(ui.screenIdle, kUiBg);
    styleScreen(ui.screenDeadline, kUiBg);
    styleScreen(ui.screenFinalHour, kUiBg);
    styleScreen(ui.screenLive, kUiBg);
    styleScreen(ui.screenPopup, kUiBg);
    styleScreen(ui.screenEvents, kUiBg);
    styleScreen(ui.screenSquad, kUiBg);

    lv_obj_remove_flag(ui.screenIdle, LV_OBJ_FLAG_SCROLLABLE);
    createHomeScreen();
    lv_obj_add_event_cb(ui.screenIdle, carouselTapEventCb, LV_EVENT_CLICKED, nullptr);

    ui.screenIdleGraph = createIdleCarouselScreen(ui.idleGraphBody);
    ui.screenIdlePrices = createIdleCarouselScreen(ui.idlePricesBody);
    ui.screenIdleWatch = createIdleCarouselScreen(ui.idleWatchBody);
    ui.screenIdleLeagues = createIdleCarouselScreen(ui.idleLeaguesBody);

    createDeadlineScreens();
    createLiveScreen();
    createPopupScreen();
    ui.eventsList = createListScreen(ui.screenEvents, "EVENTS", kEventsListH, ui.eventsDots);
    ui.eventsClearBtn = createPillButton(ui.screenEvents, "CLEAR", UI_FONT_PILL, clearEventsCb);
    lv_obj_set_style_pad_hor(ui.eventsClearBtn, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ui.eventsClearBtn, 8, LV_PART_MAIN);
    lv_obj_align(ui.eventsClearBtn, LV_ALIGN_TOP_MID, 0, kClearButtonY);
    lv_obj_add_flag(ui.eventsClearBtn, LV_OBJ_FLAG_HIDDEN);
    ui.squadList = createListScreen(ui.screenSquad, "MY SQUAD", kSquadListH, ui.squadDots);

    lv_screen_load(ui.screenIdle);
    currentMode = UiMode::Idle;

    uiBatteryCreate();
    batterySampleCb(nullptr);
    lv_timer_create(batterySampleCb, kBatterySampleMs, nullptr);
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
    ledRingSetLiveState(true, true);
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
    ledRingSetLiveState(false, false);
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

    static DeviceConfig config;  // static: keeps the credentials off the task stack
    deviceConfigGet(config);
    if (!config.wifiSsid[0]) {
        Serial.println("No WiFi network configured");
        return false;
    }
    WiFi.setHostname(kWebHostname);
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifiSsid, config.wifiPassword);

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

// Lock-free, so it never reports "off" just because another task holds demoMutex.
static bool isDemoModeEnabled() {
    return demoEnabledFlag;
}

// Caller holds demoMutex.
static void setDemoEnabledLocked(bool enabled) {
    demoState.enabled = enabled;
    demoEnabledFlag = enabled;
}

// GW points from the demo squad (plus any manual adjustment); the total moves with them. Caller holds demoMutex.
static void demoRecomputePointsLocked() {
    demoState.gwPoints = computeGwPointsFromPicks(demoState.picks, demoState.pickCount) + demoState.pointsAdjust;
    demoState.totalPoints = demoState.seededTotalPoints + (demoState.gwPoints - demoState.seededGwPoints);
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
    ledRingSetLiveState(state.isLiveGw, state.isLiveGw);  // a live demo gameweek counts as a match in play

    char gwStateBuf[48];
    snprintf(gwStateBuf, sizeof(gwStateBuf), "GW live: %s | next: %d", state.isLiveGw ? "yes" : "no", state.nextGw);
    setSharedGwStateText(gwStateBuf);
    setSharedFreshness(state.stale, millis());
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

static void printDeviceHelp() {
    Serial.println("\nDevice:");
    Serial.println("  device                        WiFi, web UI address, firmware version");
    Serial.println("  device setup                  restart as a WiFi access point for setup");
    Serial.println("  device password clear         remove the web UI password");
    Serial.println("  device touch [on|off]         off ignores the touchscreen until restart");
    Serial.println("  device restart");
    Serial.println("  device off                    switch off (on battery; USB keeps it running)");
    Serial.println("  device battery [<0-100>|auto] battery voltage; a percentage previews the icon");
    Serial.println();
}

// Returns true if the line was a device command.
static bool handleDeviceCommand(char **tokens, size_t tokenCount) {
    if (strcmp(tokens[0], "device") != 0) {
        return false;
    }
    if (tokenCount == 1) {
        static DeviceConfig config;
        deviceConfigGet(config);
        Serial.printf("[DEVICE] firmware %s | entry %d | web password %s\n", FPL_FIRMWARE_VERSION, config.entryId,
                      config.webPassword[0] ? "set" : "not set");
        Serial.printf("[DEVICE] internal heap free %u, largest %u | web task stack unused %d\n",
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                      webServerStackHighWaterMark());
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[DEVICE] WiFi '%s' %d dBm | web UI http://%s.local/ (http://%s/)\n", config.wifiSsid,
                          WiFi.RSSI(), kWebHostname, WiFi.localIP().toString().c_str());
        } else {
            Serial.printf("[DEVICE] WiFi '%s' not connected\n", config.wifiSsid);
        }
    } else if (strcmp(tokens[1], "battery") == 0) {
        if (tokenCount >= 3) {
            batteryTestPercent = strcmp(tokens[2], "auto") == 0 ? -1 : constrain(atoi(tokens[2]), 0, 100);
        }
        const float volts = batteryVolts();
        Serial.printf("[BAT] %.2f V now, %.2f V smoothed, ~%d%%%s\n", volts, batterySmoothedVolts,
                      batteryPercent(batterySmoothedVolts),
                      batteryTestPercent >= 0 ? " (icon previewing a test level)" : "");
    } else if (strcmp(tokens[1], "setup") == 0) {
        deviceConfigRequestSetupMode(true);
        Serial.println("[DEVICE] restarting in setup mode");
        delay(200);
        ESP.restart();
    } else if (strcmp(tokens[1], "password") == 0 && tokenCount >= 3 && strcmp(tokens[2], "clear") == 0) {
        deviceConfigSetWebPassword("");
        Serial.println("[DEVICE] web UI password removed");
    } else if (strcmp(tokens[1], "touch") == 0) {
        if (tokenCount >= 3) {
            touchIgnored = strcmp(tokens[2], "off") == 0;
        }
        Serial.printf("[DEVICE] touchscreen %s\n", touchIgnored ? "ignored until restart" : "on");
    } else if (strcmp(tokens[1], "restart") == 0) {
        ESP.restart();
    } else if (strcmp(tokens[1], "off") == 0) {
        display.setBacklight(0);
        powerOff();
    } else {
        printDeviceHelp();
    }
    return true;
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
        printDeviceHelp();
        return;
    }
    if (handleDisplayCommand(tokens, tokenCount) || handleDeviceCommand(tokens, tokenCount)) {
        return;
    }
    if (strcmp(tokens[0], "fps") == 0) {
        fpsRequested = true;
        return;
    }
    if (strcmp(tokens[0], "screenshot") == 0) {
        // screenshot [rank|graph|prices|watch|leagues|events|squad]
        static const char *const kPages[] = {"rank", "graph", "prices", "watch", "leagues", "events", "squad", "squadend"};
        int page = -1;
        for (int i = 0; tokenCount > 1 && i < static_cast<int>(sizeof(kPages) / sizeof(kPages[0])); ++i) {
            if (strcmp(tokens[1], kPages[i]) == 0) {
                page = i;
            }
        }
        screenshotPage = page;
        screenshotRequested = true;
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
            demoState.source = DemoSource::None;
            demoState.pointsAdjust = 0;
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
            setDemoEnabledLocked(true);
            updated = demoState;
            xSemaphoreGive(demoMutex);
            publishDemoStateToUi(updated);
            setSharedStatus("Demo mode active", 0x00E5FF);
            printDemoStateSummary(updated);
            return;
        }

        if (strcmp(tokens[1], "off") == 0) {
            if (!demoStop()) {
                Serial.println("[DEMO] Failed: demo state mutex timeout");
                return;
            }
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
            demoState.pointsAdjust = 0;
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
        demoRecomputePointsLocked();
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

// The screen the gameweek state calls for, ignoring overlays (popup, events list, squad).
static UiMode modeForGameweekState(const SharedUiState &state) {
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

static UiMode determineAutoMode(const SharedUiState &state) {
    if (currentMode == UiMode::EventPopup) {
        return currentMode;
    }
    return modeForGameweekState(state);
}

static void updateModeUi(const SharedUiState &state, const UiRuntimeState &runtime) {
    updateHomeScreen(state);
    updateGameweekScreens(state, runtime);

    if (currentMode == UiMode::Live && livePage == LivePage::Events && renderedEventsVersion != runtime.eventVersion) {
        refreshEventsList(runtime);
        renderedEventsVersion = runtime.eventVersion;
    }
    if (currentMode == UiMode::Live && livePage == LivePage::Squad && renderedSquadVersion != runtime.squadVersion) {
        refreshSquadList(runtime);
        renderedSquadVersion = runtime.squadVersion;
    }
}

static void showPopupEvent(const UiEventItem &event) {
    fillPopup(event);
    loadMode(UiMode::EventPopup, LV_SCR_LOAD_ANIM_FADE_ON);
    ledRingTriggerNotificationForMs(kPopupDisplayDurationMs);
    popupHideAtMs = millis() + kPopupDisplayDurationMs;
}

// ---------------------------------------------------------------------------
// Web demo (the Demo tab at http://fpl-buddy.local)
// Drives the display from made-up FPL data so any screen can be shown on demand, e.g. for filming: the
// between-gameweeks carousel, deadline countdowns, the live gameweek, point popups, rank moves and stale data.
// It publishes through the same shared state as a real poll, so the screens and the LED ring behave as they do
// on a matchday. FPL polling pauses while it runs. Commands arrive on the web task; anything that touches LVGL
// is handed to uiTask through demoState.screenRequest.
// ---------------------------------------------------------------------------
enum class DemoScreen : int8_t { Live = 0, Events, Squad, Rank, Graph, Prices, Watchlist, Leagues, Next };
static const char *const kDemoScreenNames[] = {"live",   "events",    "squad",   "rank", "graph",
                                               "prices", "watchlist", "leagues", "next"};

static constexpr time_t kDemoIdleDeadlineSec = 3 * 24 * 3600;  // "between gameweeks": next deadline in 3 days
static constexpr time_t kDemoNextDeadlineSec = 7 * 24 * 3600;  // next deadline after kick-off
static constexpr time_t kDemoKickoffDelaySec = 2;               // deadline passed -> live, like the next poll
static constexpr int32_t kDemoMaxDeadlineSec = 30 * 24 * 3600;
static constexpr int kDemoMaxRank = 12000000;
static constexpr int kDemoDefaultRank = 184392;
static constexpr int kDemoSampleElementBase = 90001;

static IdleScreensData *demoRealIdle = nullptr;   // PSRAM; the real between-gameweeks data, restored at stop
static volatile bool demoIdleOverridden = false;  // idleScreensData holds demo data (set under idleScreensMutex)

struct DemoSamplePlayer {
    const char *name;
    const char *team;  // kit slug
    uint8_t elementType;
    uint8_t cost;  // tenths of a million
};
// "Start with sample team": 1-3-4-3, captain Haaland (slot 9), vice captain Wirtz (slot 5).
static const DemoSamplePlayer kDemoSampleSquad[] = {
    {"Raya", "arsenal", 1, 60},         {"Gabriel", "arsenal", 2, 65},     {"Virgil", "liverpool", 2, 60},
    {"Gvardiol", "man_city", 2, 60},    {"Wirtz", "liverpool", 3, 85},     {"Palmer", "chelsea", 3, 105},
    {"Saka", "arsenal", 3, 100},        {"B.Fernandes", "man_utd", 3, 90}, {"Haaland", "man_city", 4, 145},
    {"Watkins", "aston_villa", 4, 90},  {"Isak", "liverpool", 4, 105},     {"Pickford", "everton", 1, 55},
    {"Munoz", "crystal_palace", 2, 55}, {"Gordon", "newcastle", 3, 75},    {"Mbeumo", "man_utd", 3, 80},
};
static constexpr int kDemoSampleCaptainSlot = 9;
static constexpr int kDemoSampleViceSlot = 5;

// The sample squad's finished gameweek, applied silently (59 points).
struct DemoSampleReturn {
    uint8_t slot;
    const char *type;
    uint8_t count;
};
static const DemoSampleReturn kDemoSampleReturns[] = {
    {1, "cs", 1},     {1, "save", 3}, {2, "cs", 1},    {3, "cs", 1},  {5, "assist", 1},
    {6, "goal", 1},   {9, "goal", 1}, {9, "bonus", 3}, {13, "cs", 1},
};

struct DemoSampleLeague {
    const char *name;
    int rank;
    int lastRank;
    int size;
};
// Used when the real entry has no mini leagues (or there is no FPL data yet).
static const DemoSampleLeague kDemoSampleLeagues[] = {
    {"Office League", 3, 5, 14}, {"Family & Friends", 1, 2, 8}, {"Sunday League Lads", 7, 4, 12}, {"Uni Mates", 2, 2, 10},
};

// Web event types -> applyDemoEventToPick types. A count of 0 means one defensive-contribution award.
struct DemoEventType {
    const char *name;
    const char *applyAs;
    int count;
};
static const DemoEventType kDemoEventTypes[] = {
    {"goal", "goal", 1}, {"assist", "assist", 1}, {"cs", "cs", 1},         {"save", "save", 3},
    {"psave", "pen_save", 1}, {"dc", "dc", 0}, {"bonus", "bonus", 3},       {"concede", "concede", 2},
    {"yc", "yc", 1},     {"rc", "rc", 1},         {"og", "og", 1},          {"pmiss", "pen_miss", 1},
};

static bool demoClockValid(time_t &now) {
    now = time(nullptr);
    return now >= kMinValidEpoch;
}

static int demoClampRank(int64_t rank) {
    return rank < 1 ? 1 : (rank > kDemoMaxRank ? kDemoMaxRank : static_cast<int>(rank));
}

static uint32_t demoRandom(uint32_t &seed) {
    seed = seed * 1664525U + 1013904223U;
    return seed >> 8;
}

// Next FPL price update, which runs at about 01:30 UK time.
static bool demoNextPriceUpdate(time_t &out) {
    time_t now = 0;
    if (!demoClockValid(now)) {
        return false;
    }
    struct tm local;
    localtime_r(&now, &local);
    local.tm_hour = 1;
    local.tm_min = 30;
    local.tm_sec = 0;
    time_t at = mktime(&local);
    if (at <= now) {
        at += 24 * 3600;
    }
    out = at;
    return true;
}

static int demoPlayerCost(const TeamPick &pick, const IdleScreensData &real) {
    for (size_t i = 0; i < real.prices.squadCount; ++i) {
        if (real.prices.squad[i].elementId == pick.elementId) {
            return real.prices.squad[i].nowCost;
        }
    }
    const int sampleIndex = pick.elementId - kDemoSampleElementBase;
    if (sampleIndex >= 0 && sampleIndex < static_cast<int>(sizeof(kDemoSampleSquad) / sizeof(kDemoSampleSquad[0]))) {
        return kDemoSampleSquad[sampleIndex].cost;
    }
    static const int kByType[] = {50, 50, 50, 70, 75};
    return kByType[(pick.elementType >= 0 && pick.elementType <= 4) ? pick.elementType : 0];
}

// Between-gameweeks screens for the demo: a rank history ending at the demo rank, price changes for the demo
// squad and the watchlist (a rise, a fall, a hold and a lock, so every state shows), and the real mini leagues
// (or sample ones). Caller holds demoMutex.
static void publishDemoIdleScreensLocked() {
    static DisplaySettings settings;  // guarded by demoMutex
    displaySettingsGet(settings);
    if (!idleScreensMutex || xSemaphoreTake(idleScreensMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    IdleScreensData &d = *idleScreensData;
    const IdleScreensData &real = *demoRealIdle;
    const DemoState &s = demoState;

    // Rank history: the last finished gameweek ends at the demo rank (the kick-off rank while live), the one
    // before it at the rank before the latest move, and earlier gameweeks wander (the same way every time).
    RankHistory &h = d.history;
    h.count = 0;
    const int lastGw = s.isLiveGw ? s.currentGw - 1 : s.currentGw;
    if (lastGw > 0) {
        const size_t count = lastGw < static_cast<int>(kMaxGameweeks) ? static_cast<size_t>(lastGw) : kMaxGameweeks;
        uint32_t seed = 0x2545F491U + static_cast<uint32_t>(lastGw);
        int64_t rank = s.isLiveGw ? static_cast<int64_t>(s.overallRank) + s.rankDiff : s.overallRank;
        for (size_t i = count; i-- > 0;) {
            h.gw[i] = lastGw - static_cast<int>(count - 1 - i);
            h.overallRank[i] = demoClampRank(rank);
            if (i == count - 1 && !s.isLiveGw) {
                rank = static_cast<int64_t>(s.overallRank) + s.rankDiff;
            } else {
                rank = rank * (85 + static_cast<int64_t>(demoRandom(seed) % 50)) / 100;
            }
        }
        h.count = count;
    }
    d.hasHistory = h.count > 0;

    PriceData &prices = d.prices;
    prices.hasNextUpdate = demoNextPriceUpdate(prices.nextUpdateUtc);
    prices.squadCount = 0;
    bool midRiser = false;
    bool benchFaller = false;
    for (size_t i = 0; i < s.pickCount && prices.squadCount < sizeof(prices.squad) / sizeof(prices.squad[0]); ++i) {
        const TeamPick &pick = s.picks[i];
        PlayerPrice &p = prices.squad[prices.squadCount++];
        p = PlayerPrice();
        p.elementId = pick.elementId;
        sanitizeUtf8ToAscii(pick.playerName.c_str(), p.name, sizeof(p.name));
        strlcpy(p.team, pick.teamShortName, sizeof(p.team));
        p.isGk = pick.elementType == 1;
        p.nowCost = demoPlayerCost(pick, real);
        // A rise for the captain and a starting midfielder, a fall for a benched outfielder; the rest hold.
        if (pick.isCaptain) {
            p.projectedPct = 1034;
        } else if (!midRiser && pick.elementType == 3 && pick.squadPosition <= 11) {
            p.projectedPct = 1008;
            midRiser = true;
        } else if (!benchFaller && pick.squadPosition > 11 && pick.elementType != 1) {
            p.projectedPct = -1012;
            benchFaller = true;
        } else {
            p.projectedPct = static_cast<int>((i * 373) % 1400) - 700;
        }
    }

    static const int kWatchPct[] = {1015, 420, -1006, 230};
    prices.watchCount = 0;
    for (size_t i = 0; i < settings.watchCount && prices.watchCount < kMaxWatchlist; ++i) {
        PlayerPrice &p = prices.watch[prices.watchCount++];
        p = PlayerPrice();
        const PlayerPrice *match = nullptr;
        for (size_t j = 0; j < real.prices.watchCount && !match; ++j) {
            if (watchEntryMatches(settings.watchlist[i], real.prices.watch[j].elementId, real.prices.watch[j].name)) {
                match = &real.prices.watch[j];
            }
        }
        if (match) {
            p = *match;
        } else {
            // No FPL data for this player yet: show the watchlist entry, "saka" -> "Saka".
            strlcpy(p.name, settings.watchlist[i], sizeof(p.name));
            if (p.name[0] >= 'a' && p.name[0] <= 'z') {
                p.name[0] = static_cast<char>(p.name[0] - 'a' + 'A');
            }
            p.nowCost = 60 + 5 * static_cast<int>(i);
        }
        p.projectedPct = kWatchPct[i % 4];
        p.locked = (i % 4) == 3;
    }
    d.hasPrices = true;

    if (real.hasLeagues && real.leagueCount > 0) {
        d.leagueCount = real.leagueCount;
        for (size_t i = 0; i < real.leagueCount; ++i) {
            d.leagues[i] = real.leagues[i];
        }
    } else {
        d.leagueCount = sizeof(kDemoSampleLeagues) / sizeof(kDemoSampleLeagues[0]);
        for (size_t i = 0; i < d.leagueCount; ++i) {
            MiniLeague &lg = d.leagues[i];
            lg = MiniLeague();
            lg.id = 900001 + static_cast<int>(i);
            strlcpy(lg.name, kDemoSampleLeagues[i].name, sizeof(lg.name));
            lg.rank = kDemoSampleLeagues[i].rank;
            lg.lastRank = kDemoSampleLeagues[i].lastRank;
            lg.size = kDemoSampleLeagues[i].size;
        }
    }
    d.hasLeagues = true;
    d.version++;
    demoIdleOverridden = true;
    xSemaphoreGive(idleScreensMutex);
}

// Shows the demo on the display, with the status line a real poll would leave. Caller holds demoMutex.
static void publishDemoLocked() {
    publishDemoStateToUi(demoState);
    publishDemoIdleScreensLocked();
    if (demoState.stale) {
        setSharedStatus("FPL fetch failed", 0xFF5A5A);
    } else {
        setSharedStatus("FPL updated", 0x38D39F);
    }
}

// Caller holds demoMutex.
static void demoSeedSampleLocked() {
    DemoState &s = demoState;
    const size_t count = sizeof(kDemoSampleSquad) / sizeof(kDemoSampleSquad[0]);
    static const char *const kPositionNames[] = {"", "GKP", "DEF", "MID", "FWD"};
    int delta = 0;
    const char *label = nullptr;
    for (size_t i = 0; i < count; ++i) {
        const DemoSamplePlayer &src = kDemoSampleSquad[i];
        TeamPick &p = s.picks[i];
        p = TeamPick();
        p.elementId = kDemoSampleElementBase + static_cast<int>(i);
        p.squadPosition = static_cast<int>(i) + 1;
        p.isCaptain = p.squadPosition == kDemoSampleCaptainSlot;
        p.isViceCaptain = p.squadPosition == kDemoSampleViceSlot;
        p.multiplier = p.squadPosition > 11 ? 0 : (p.isCaptain ? 2 : 1);
        p.elementType = src.elementType;
        p.playerName = src.name;
        p.positionName = kPositionNames[src.elementType];
        strlcpy(p.teamShortName, src.team, sizeof(p.teamShortName));
        if (p.squadPosition <= 11) {
            applyDemoEventToPick(p, "mins", 90, delta, label);
        }
    }
    s.pickCount = count;
    for (const DemoSampleReturn &r : kDemoSampleReturns) {
        if (TeamPick *p = findPickBySquadSlot(s.picks, s.pickCount, r.slot)) {
            applyDemoEventToPick(*p, r.type, r.count, delta, label);
        }
    }
}

// Seeds the demo from the FPL cache (your team, as last polled) or the sample squad. Caller holds fplDataMutex
// and demoMutex.
static void demoSeedLocked(DemoSource source) {
    DemoState &s = demoState;
    const FplDataCache &c = *fplCache;
    time_t now = 0;
    const bool clockValid = demoClockValid(now);
    s.pointsAdjust = 0;
    if (source == DemoSource::MyTeam) {
        s.pickCount = c.pickCount;
        for (size_t i = 0; i < c.pickCount; ++i) {
            s.picks[i] = c.picks[i];
        }
        s.currentGw = c.entry.currentGw > 0 ? c.entry.currentGw : 1;
        s.isLiveGw = c.hasGwState && c.gwState.isLiveGw;
        s.nextGw = c.hasGwState ? c.gwState.nextGw : 0;
        s.hasNextGw = s.nextGw > 0;
        s.hasDeadline = c.hasGwState && c.gwState.hasDeadline;
        s.deadlineUtc = c.gwState.deadlineUtc;
        s.overallRank = c.entry.overallRank;
        s.rankDiff = (c.entry.overallRank > 0 && c.entry.previousOverallRank > 0)
                         ? c.entry.previousOverallRank - c.entry.overallRank
                         : 0;
        s.gwPoints = computeGwPointsFromPicks(s.picks, s.pickCount);
        s.totalPoints = c.entry.overallPoints;
    } else {
        const int gw = (c.hasEntry && c.entry.currentGw > 0) ? c.entry.currentGw : 7;
        demoSeedSampleLocked();
        s.currentGw = gw;
        s.isLiveGw = false;
        s.nextGw = gw < 38 ? gw + 1 : 0;
        s.hasNextGw = s.nextGw > 0;
        s.hasDeadline = s.hasNextGw && clockValid;
        s.deadlineUtc = now + kDemoIdleDeadlineSec;
        s.overallRank = kDemoDefaultRank;
        s.rankDiff = 46613;
        s.gwPoints = computeGwPointsFromPicks(s.picks, s.pickCount);
        s.totalPoints = s.gwPoints + 55 * (gw - 1);
    }
    if (s.overallRank <= 0) {  // a new team with no rank yet
        s.overallRank = kDemoDefaultRank;
        s.rankDiff = 0;
    }
    s.hasRankData = true;
    s.seededGwPoints = s.gwPoints;
    s.seededTotalPoints = s.totalPoints;
    for (size_t i = 0; i < s.pickCount; ++i) {
        s.seededPicks[i] = s.picks[i];
    }
    s.seeded = true;
    s.source = source;
    s.stale = false;
    s.screenRequest = -1;
}

// The next gameweek goes live with no points yet. Caller holds demoMutex.
static void demoKickoffLocked(time_t now) {
    DemoState &s = demoState;
    s.currentGw = (s.hasNextGw && s.nextGw > 0) ? s.nextGw : s.currentGw + 1;
    if (s.currentGw > 38) {
        s.currentGw = 1;
    }
    s.nextGw = s.currentGw < 38 ? s.currentGw + 1 : 0;
    s.hasNextGw = s.nextGw > 0;
    s.isLiveGw = true;
    s.hasDeadline = s.hasNextGw;
    s.deadlineUtc = now + kDemoNextDeadlineSec;
    for (size_t i = 0; i < s.pickCount; ++i) {
        s.picks[i].live = TeamPick::LiveStats();
    }
    s.pointsAdjust = 0;
    s.seededTotalPoints = s.totalPoints;
    s.seededGwPoints = 0;
    demoRecomputePointsLocked();
    s.rankDiff = 0;
    clearUiEvents();
}

// Caller holds demoMutex.
static void demoEndGameweekLocked() {
    DemoState &s = demoState;
    s.isLiveGw = false;
    s.nextGw = s.currentGw < 38 ? s.currentGw + 1 : 1;
    s.hasNextGw = true;
}

// Moves the overall rank for `points` gained (or lost) by the squad: about 1.5% per point. Caller holds
// demoMutex.
static void demoMoveRankLocked(int points) {
    DemoState &s = demoState;
    if (points == 0 || s.overallRank <= 0) {
        return;
    }
    float factor = 1.0f - 0.015f * static_cast<float>(points);
    factor = factor < 0.6f ? 0.6f : (factor > 1.4f ? 1.4f : factor);
    int next = demoClampRank(static_cast<int64_t>(static_cast<float>(s.overallRank) * factor + 0.5f));
    if (next == s.overallRank) {
        next = demoClampRank(static_cast<int64_t>(next) + (points > 0 ? -1 : 1));
    }
    s.rankDiff += s.overallRank - next;
    s.overallRank = next;
}

// Applies a scoring event to a demo player and queues the popups a live poll would show, with "PLAYING!" and
// "60+ mins!" first when the event implies them. `effectiveOut` is the change in the squad's points (after the
// multiplier). Returns false, leaving the player unchanged, for an unknown event or one that scores nothing for
// this player. Caller holds demoMutex.
static bool demoApplyPlayerEvent(TeamPick &pick, const char *type, int count, int &effectiveOut, const char *&error) {
    struct Step {
        int delta;
        int totalAfter;
        const char *label;
    };
    Step steps[3];
    size_t stepCount = 0;
    const TeamPick::LiveStats before = pick.live;
    auto apply = [&](const char *applyAs, int n, const char *label) {
        int delta = 0;
        const char *defaultLabel = nullptr;
        applyDemoEventToPick(pick, applyAs, n, delta, defaultLabel);
        if (delta != 0 && stepCount < 3) {
            steps[stepCount++] = {delta, pick.live.totalPoints, label ? label : defaultLabel};
        }
        return delta;
    };

    const bool playing = strcmp(type, "playing") == 0;
    const bool sixty = strcmp(type, "sixty") == 0;
    const DemoEventType *event = nullptr;
    for (const DemoEventType &e : kDemoEventTypes) {
        if (strcmp(type, e.name) == 0) {
            event = &e;
        }
    }
    if (!playing && !sixty && !event) {
        error = "Unknown event";
        return false;
    }

    int mainDelta = 0;
    if (pick.live.minutes < 1) {
        mainDelta = apply("mins", 1, "PLAYING!");
    }
    if ((sixty || (event && strcmp(event->name, "cs") == 0)) && pick.live.minutes < 60) {
        mainDelta = apply("mins", 60 - pick.live.minutes, "60+ mins!");
    }
    if (event) {
        int n = (count > 0 && count <= 10) ? count : event->count;
        if (n <= 0) {
            n = defensiveContributionThresholdForElementType(pick.elementType);
            n = n > 0 ? n : 1;
        }
        mainDelta = apply(event->applyAs, n, nullptr);
    }
    if (mainDelta == 0) {
        pick.live = before;
        error = playing ? "Already playing" : (sixty ? "Already past 60 minutes" : "That scores no points for this player");
        return false;
    }

    int total = 0;
    for (size_t i = 0; i < stepCount; ++i) {
        total += steps[i].delta;
        pushPickEvent(pick, steps[i].delta, steps[i].label, steps[i].totalAfter);
    }
    effectiveOut = total * pick.multiplier;
    return true;
}

// Starts (or restarts) the demo. Web task.
static bool demoStart(DemoSource source, const char *&error) {
    if (!demoMutex || !fplDataMutex || xSemaphoreTake(demoMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        error = "Busy, try again";
        return false;
    }
    // Take over the display first: fplTask stops publishing once it sees the flag, and taking the cache below
    // waits out a poll that is already running.
    const bool wasEnabled = demoState.enabled;
    setDemoEnabledLocked(true);
    xSemaphoreGive(demoMutex);

    auto fail = [&](const char *message) {
        if (!wasEnabled && xSemaphoreTake(demoMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            setDemoEnabledLocked(false);
            xSemaphoreGive(demoMutex);
            fplPollRequested = true;  // a poll may have skipped the display while the flag was set
        }
        error = message;
        return false;
    };
    if (xSemaphoreTake(fplDataMutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        return fail("An FPL update is running, try again");
    }
    if (source == DemoSource::MyTeam && !(fplCache->hasEntry && fplCache->pickCount > 0)) {
        xSemaphoreGive(fplDataMutex);
        return fail("No FPL data yet: wait for the first update, or use the sample team");
    }
    if (xSemaphoreTake(demoMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        xSemaphoreGive(fplDataMutex);
        return fail("Busy, try again");
    }
    if (!demoIdleOverridden && xSemaphoreTake(idleScreensMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *demoRealIdle = *idleScreensData;
        xSemaphoreGive(idleScreensMutex);
    }
    demoSeedLocked(source);
    xSemaphoreGive(fplDataMutex);
    clearUiEvents();
    publishDemoLocked();
    Serial.printf("[DEMO] started with %s: GW%d %s, %d pts, rank %d\n",
                  source == DemoSource::Sample ? "the sample team" : "my team", demoState.currentGw,
                  demoState.isLiveGw ? "live" : "finished", demoState.gwPoints, demoState.overallRank);
    xSemaphoreGive(demoMutex);
    return true;
}

// Ends the demo; fplTask then shows the cached FPL data and polls. False if the demo state is busy.
static bool demoStop() {
    if (!demoMutex || xSemaphoreTake(demoMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return false;
    }
    const bool wasEnabled = demoState.enabled;
    setDemoEnabledLocked(false);
    demoState.stale = false;
    demoState.screenRequest = -1;
    xSemaphoreGive(demoMutex);

    demoCarouselPaused = false;
    if (demoIdleOverridden && xSemaphoreTake(idleScreensMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const uint32_t version = idleScreensData->version;
        *idleScreensData = *demoRealIdle;
        idleScreensData->version = version + 1;
        demoIdleOverridden = false;
        xSemaphoreGive(idleScreensMutex);
    }
    if (wasEnabled) {
        clearUiEvents();
        setSharedStatus("Fetching FPL points...", 0xFFCC66);
        Serial.println("[DEMO] stopped, live polling resumed");
    }
    return true;
}

// A command other than start/restart/stop. Caller holds demoMutex; the demo is running.
static bool demoCommandLocked(const char *action, JsonObjectConst cmd, const char *&error) {
    DemoState &s = demoState;
    time_t now = 0;
    const bool clockValid = demoClockValid(now);

    if (strcmp(action, "screen") == 0) {
        const char *name = cmd["screen"] | "";
        for (size_t i = 0; i < sizeof(kDemoScreenNames) / sizeof(kDemoScreenNames[0]); ++i) {
            if (strcmp(name, kDemoScreenNames[i]) == 0) {
                s.screenRequest = static_cast<int>(i);
                return true;
            }
        }
        error = "Unknown screen";
        return false;
    }

    if (strcmp(action, "phase") == 0) {
        const char *phase = cmd["phase"] | "";
        if (!clockValid) {
            error = "The clock is not synced yet";
            return false;
        }
        if (strcmp(phase, "live") == 0) {
            if (!s.isLiveGw) {
                demoKickoffLocked(now);
            }
        } else if (strcmp(phase, "idle") == 0 || strcmp(phase, "deadline") == 0) {
            const int32_t seconds = phase[0] == 'i' ? static_cast<int32_t>(kDemoIdleDeadlineSec) : (cmd["seconds"] | -1);
            if (seconds < 0 || seconds > kDemoMaxDeadlineSec) {
                error = "The deadline must be 0 seconds to 30 days away";
                return false;
            }
            if (s.isLiveGw) {
                demoEndGameweekLocked();
            }
            if (!s.hasNextGw) {
                s.nextGw = s.currentGw < 38 ? s.currentGw + 1 : 1;
                s.hasNextGw = true;
            }
            s.hasDeadline = true;
            s.deadlineUtc = now + seconds;
        } else {
            error = "Unknown phase";
            return false;
        }
        publishDemoLocked();
        return true;
    }

    if (strcmp(action, "set") == 0) {
        // Validate everything before changing anything.
        const int gw = cmd["gw"] | s.currentGw;
        const int gwPoints = cmd["gwPoints"] | s.gwPoints;
        const int totalPoints = cmd["totalPoints"] | s.totalPoints;
        const int rank = cmd["rank"] | s.overallRank;
        const int rankDiff = cmd["rankDiff"] | s.rankDiff;
        if (gw < 1 || gw > 38) {
            error = "The gameweek must be 1-38";
            return false;
        }
        if (gwPoints < -99 || gwPoints > 999 || totalPoints < 0 || totalPoints > 9999) {
            error = "Points out of range";
            return false;
        }
        if (rank < 1 || rank > kDemoMaxRank || rankDiff < -kDemoMaxRank || rankDiff > kDemoMaxRank) {
            error = "Rank out of range";
            return false;
        }

        if (cmd.containsKey("carouselPaused")) {
            demoCarouselPaused = cmd["carouselPaused"] | false;
        }
        s.autoKickoff = cmd["autoKickoff"] | s.autoKickoff;
        s.rankFollowsPoints = cmd["rankFollows"] | s.rankFollowsPoints;
        const bool stale = cmd["stale"] | s.stale;
        const bool dataChanged = gw != s.currentGw || gwPoints != s.gwPoints || totalPoints != s.totalPoints ||
                                 rank != s.overallRank || rankDiff != s.rankDiff || stale != s.stale;
        if (!dataChanged) {
            return true;
        }
        if (gw != s.currentGw) {
            s.currentGw = gw;
            s.nextGw = gw < 38 ? gw + 1 : 1;
            s.hasNextGw = true;
        }
        if (gwPoints != s.gwPoints) {
            s.pointsAdjust += gwPoints - s.gwPoints;
            demoRecomputePointsLocked();
        }
        if (totalPoints != s.totalPoints) {
            s.seededTotalPoints += totalPoints - s.totalPoints;
            s.totalPoints = totalPoints;
        }
        s.overallRank = rank;
        s.rankDiff = rankDiff;
        s.hasRankData = true;
        s.stale = stale;
        publishDemoLocked();
        return true;
    }

    if (strcmp(action, "event") == 0) {
        TeamPick *pick = findPickBySquadSlot(s.picks, s.pickCount, cmd["slot"] | 0);
        if (!pick) {
            error = "Unknown player";
            return false;
        }
        int effective = 0;
        if (!demoApplyPlayerEvent(*pick, cmd["type"] | "", cmd["count"] | 0, effective, error)) {
            return false;
        }
        if (s.rankFollowsPoints) {
            demoMoveRankLocked(effective);
        }
        demoRecomputePointsLocked();
        publishDemoLocked();
        return true;
    }

    if (strcmp(action, "clearScores") == 0) {
        for (size_t i = 0; i < s.pickCount; ++i) {
            s.picks[i].live = TeamPick::LiveStats();
        }
        s.pointsAdjust = 0;
        demoRecomputePointsLocked();
        clearUiEvents();
        publishDemoLocked();
        return true;
    }

    error = "Unknown action";
    return false;
}

// uiTask: shows a screen the web UI asked for. Each carousel's pages only show in its part of the gameweek
// (events and squad while it is live, prices and watchlist between gameweeks), and only when they have something
// to show.
static void demoShowScreen(DemoScreen screen) {
    if (currentMode == UiMode::EventPopup) {
        SharedUiState state;
        if (!readSharedUiState(state)) {
            return;
        }
        popupHideAtMs = 0;
        loadMode(modeForGameweekState(state), LV_SCR_LOAD_ANIM_FADE_ON);
    }
    if (currentMode == UiMode::Live) {
        LivePage page = LivePage::Main;
        switch (screen) {
            case DemoScreen::Next: advanceLivePage(); return;
            case DemoScreen::Events: page = LivePage::Events; break;
            case DemoScreen::Squad: page = LivePage::Squad; break;
            case DemoScreen::Leagues: page = LivePage::Leagues; break;
            case DemoScreen::Graph: page = LivePage::RankGraph; break;
            default: break;
        }
        if (livePageAvailable(page)) {
            carouselSubPage = 0;
            showLivePage(page);
        }
        return;
    }
    if (screen == DemoScreen::Live || currentMode != UiMode::Idle) {
        return;
    }
    if (screen == DemoScreen::Next) {
        advanceIdlePage();
        return;
    }
    IdlePage page = IdlePage::Rank;
    switch (screen) {
        case DemoScreen::Graph: page = IdlePage::RankGraph; break;
        case DemoScreen::Prices: page = IdlePage::Prices; break;
        case DemoScreen::Watchlist: page = IdlePage::Watchlist; break;
        case DemoScreen::Leagues: page = IdlePage::Leagues; break;
        default: break;
    }
    if (idlePageAvailable(page)) {
        carouselSubPage = 0;
        showIdlePage(page);
    }
}

// uiTask: screen requests, and the gameweek going live once a web demo's deadline has passed.
static void serviceWebDemo() {
    if (!demoEnabledFlag || !demoMutex || xSemaphoreTake(demoMutex, 0) != pdTRUE) {
        return;
    }
    const int screen = demoState.screenRequest;
    demoState.screenRequest = -1;
    time_t now = 0;
    if (demoState.enabled && demoState.source != DemoSource::None && demoState.autoKickoff && !demoState.isLiveGw &&
        demoState.hasDeadline && demoClockValid(now) && now >= demoState.deadlineUtc + kDemoKickoffDelaySec) {
        demoKickoffLocked(now);
        publishDemoLocked();
        Serial.printf("[DEMO] deadline passed, GW%d live\n", demoState.currentGw);
    }
    xSemaphoreGive(demoMutex);
    if (screen >= 0) {
        demoShowScreen(static_cast<DemoScreen>(screen));
    }
}

static const char *demoScreenName() {
    switch (currentMode) {
        case UiMode::Idle:
            switch (idlePage) {
                case IdlePage::RankGraph: return "graph";
                case IdlePage::Prices: return "prices";
                case IdlePage::Watchlist: return "watchlist";
                case IdlePage::Leagues: return "leagues";
                default: return "rank";
            }
        case UiMode::Deadline: return "deadline";
        case UiMode::FinalHour: return "final";
        case UiMode::Live:
            switch (livePage) {
                case LivePage::Events: return "events";
                case LivePage::Squad: return "squad";
                case LivePage::Leagues: return "leagues";
                case LivePage::RankGraph: return "graph";
                default: return "live";
            }
        case UiMode::EventPopup: return "popup";
    }
    return "rank";
}

void appDemoStateJson(String &out) {
    DynamicJsonDocument doc(6144);
    time_t now = 0;
    const bool clockValid = demoClockValid(now);
    doc["hasRealData"] = fplCache && fplCache->hasEntry && fplCache->pickCount > 0;
    doc["clock"] = clockValid;
    doc["screen"] = demoScreenName();
    doc["carouselPaused"] = static_cast<bool>(demoCarouselPaused);
    if (!demoMutex || xSemaphoreTake(demoMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        doc["enabled"] = static_cast<bool>(demoEnabledFlag);
        serializeJson(doc, out);
        return;
    }
    const DemoState &s = demoState;
    doc["enabled"] = s.enabled;
    doc["source"] = s.source == DemoSource::MyTeam ? "mine" : (s.source == DemoSource::Sample ? "sample" : "serial");
    if (s.enabled) {
        doc["gw"] = s.currentGw;
        doc["nextGw"] = s.hasNextGw ? s.nextGw : 0;
        doc["live"] = s.isLiveGw;
        if (s.hasDeadline && clockValid) {
            doc["deadlineIn"] = static_cast<long>(s.deadlineUtc - now);
        } else {
            doc["deadlineIn"] = nullptr;
        }
        doc["gwPoints"] = s.gwPoints;
        doc["totalPoints"] = s.totalPoints;
        doc["rank"] = s.hasRankData ? s.overallRank : 0;
        doc["rankDiff"] = s.rankDiff;
        doc["stale"] = s.stale;
        doc["autoKickoff"] = s.autoKickoff;
        doc["rankFollows"] = s.rankFollowsPoints;
        JsonArray squad = doc.createNestedArray("squad");
        for (size_t i = 0; i < s.pickCount; ++i) {
            const TeamPick &p = s.picks[i];
            bool projectedBonusAdded = false;
            bool bonusIncluded = false;
            JsonObject row = squad.createNestedObject();
            row["slot"] = p.squadPosition;
            row["name"] = p.playerName.length() ? p.playerName.c_str() : "unknown";
            row["team"] = p.teamShortName;
            row["pos"] = p.elementType;
            row["pts"] = adjustedLivePointsWithProjectedBonus(p, projectedBonusAdded, bonusIncluded);
            row["mult"] = p.multiplier;
            row["played"] = p.live.minutes > 0;
            row["c"] = p.isCaptain;
            row["vc"] = p.isViceCaptain;
            row["bench"] = p.squadPosition > 11;
        }
    }
    serializeJson(doc, out);  // before releasing demoMutex: the names point into demoState
    xSemaphoreGive(demoMutex);
}

bool appDemoCommand(JsonObjectConst cmd, const char *&error) {
    error = nullptr;
    const char *action = cmd["action"] | "";
    if (strcmp(action, "start") == 0) {
        const char *source = cmd["source"] | "mine";
        return demoStart(strcmp(source, "sample") == 0 ? DemoSource::Sample : DemoSource::MyTeam, error);
    }
    if (strcmp(action, "restart") == 0) {
        DemoSource source = DemoSource::MyTeam;
        if (demoMutex && xSemaphoreTake(demoMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (demoState.source == DemoSource::Sample) {
                source = DemoSource::Sample;
            }
            xSemaphoreGive(demoMutex);
        }
        return demoStart(source, error);
    }
    if (strcmp(action, "stop") == 0) {
        if (!demoStop()) {
            error = "Busy, try again";
            return false;
        }
        return true;
    }
    if (!demoMutex || xSemaphoreTake(demoMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        error = "Busy, try again";
        return false;
    }
    bool ok = false;
    if (!demoState.enabled || !demoState.seeded) {
        error = "Start the demo first";
    } else {
        ok = demoCommandLocked(action, cmd, error);
    }
    xSemaphoreGive(demoMutex);
    return ok;
}

// --- Setup mode: hold to enter, on-screen instructions --------------------------------------------------------

// Set by runSetupMode (fplTask). The strings are written before the flag, and never change afterwards.
static volatile bool setupModeActive = false;
static char setupApName[24] = "";
static char setupApIp[16] = "";

static struct {
    lv_obj_t *countdown = nullptr;
    lv_obj_t *countdownTitle = nullptr;
    lv_obj_t *countdownNumber = nullptr;
    lv_obj_t *countdownArc = nullptr;
    lv_obj_t *info = nullptr;
} setupUi;

static lv_obj_t *createSetupOverlay() {
    lv_obj_t *panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, kDisplayWidth, kDisplayHeight);
    lv_obj_center(panel);
    styleScreen(panel, kColorBgDeep);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void createSetupCountdown() {
    setupUi.countdown = createSetupOverlay();

    setupUi.countdownArc = lv_arc_create(setupUi.countdown);
    lv_obj_set_size(setupUi.countdownArc, kDisplayWidth, kDisplayHeight);
    lv_obj_center(setupUi.countdownArc);
    lv_arc_set_bg_angles(setupUi.countdownArc, 0, 360);
    lv_arc_set_rotation(setupUi.countdownArc, 270);
    lv_arc_set_range(setupUi.countdownArc, 0, 100);
    lv_obj_set_style_arc_width(setupUi.countdownArc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(setupUi.countdownArc, lv_color_hex(kColorAccentCyan), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(setupUi.countdownArc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(setupUi.countdownArc, lv_color_hex(kColorBgSurface), LV_PART_MAIN);
    lv_obj_remove_style(setupUi.countdownArc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(setupUi.countdownArc, LV_OBJ_FLAG_CLICKABLE);

    setupUi.countdownTitle = createLabel(setupUi.countdown, kFontCaption, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(setupUi.countdownTitle, LV_ALIGN_CENTER, 0, -56);

    setupUi.countdownNumber = createLabel(setupUi.countdown, kFontHero, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(setupUi.countdownNumber, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *hint = createLabel(setupUi.countdown, kFontMicro, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(hint, "Release to cancel");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 64);
}

static void createSetupInfo() {
    setupUi.info = createSetupOverlay();

    lv_obj_t *title = createLabel(setupUi.info, kFontCaption, kColorAccentCyan, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(title, "SETUP MODE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);

    // Standard WiFi QR payload: phone cameras offer to join the network.
    char payload[64];
    snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", setupApName);
    lv_obj_t *qr = lv_qrcode_create(setupUi.info);
    lv_qrcode_set_size(qr, 132);
    lv_qrcode_set_dark_color(qr, lv_color_hex(kColorBgDeep));
    lv_qrcode_set_light_color(qr, lv_color_hex(kColorTextPrimary));
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_update(qr, payload, strlen(payload));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t *join = createLabel(setupUi.info, kFontMicro, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(join, "Scan with your phone camera, or join WiFi");
    lv_obj_align(join, LV_ALIGN_TOP_MID, 0, 226);

    lv_obj_t *name = createLabel(setupUi.info, kFontBody, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(name, setupApName);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 248);

    lv_obj_t *openHint = createLabel(setupUi.info, kFontMicro, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text_fmt(openHint, "The setup page opens by itself,\nor go to %s", setupApIp);
    lv_obj_align(openHint, LV_ALIGN_TOP_MID, 0, 282);

    lv_obj_t *exitHint = createLabel(setupUi.info, kFontMicro, kColorAccentAmber, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text_fmt(exitHint, "Hold the screen %u s to exit", static_cast<unsigned>(kSetupHoldTriggerMs / 1000));
    lv_obj_align(exitHint, LV_ALIGN_TOP_MID, 0, 334);
}

static void serviceSetupHold() {
    if (setupModeActive && !setupUi.info) {
        createSetupInfo();
    }

    // Setting a label's text redraws it even when unchanged, so only touch the widgets when a value changes.
    static bool countdownShown = false;
    static unsigned shownSeconds = 0;
    static int shownProgress = -1;

    const uint32_t heldMs = touchPressStartMs != 0 ? millis() - touchPressStartMs : 0;
    if (heldMs < kSetupHoldPromptMs) {
        if (countdownShown) {
            lv_obj_add_flag(setupUi.countdown, LV_OBJ_FLAG_HIDDEN);
            countdownShown = false;
        }
        return;
    }

    if (!setupUi.countdown) {
        createSetupCountdown();
    }
    if (!countdownShown) {
        lv_label_set_text(setupUi.countdownTitle, setupModeActive ? "Leaving setup mode in" : "Entering setup mode in");
        lv_obj_remove_flag(setupUi.countdown, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(setupUi.countdown);
        countdownShown = true;
        shownSeconds = 0;
        shownProgress = -1;
    }

    if (heldMs >= kSetupHoldTriggerMs) {
        lv_label_set_text(setupUi.countdownTitle, setupModeActive ? "Leaving setup mode" : "Starting setup mode");
        lv_label_set_text(setupUi.countdownNumber, "...");
        lv_arc_set_value(setupUi.countdownArc, 100);
        lv_refr_now(lvglDisp);
        // runSetupMode already cleared the flag, so a plain restart leaves setup mode.
        if (!setupModeActive) {
            deviceConfigRequestSetupMode(true);
        }
        Serial.printf("[SETUP] screen held %u s, restarting %s setup mode\n",
                      static_cast<unsigned>(kSetupHoldTriggerMs / 1000), setupModeActive ? "out of" : "into");
        delay(100);
        ESP.restart();
    }

    const unsigned seconds = static_cast<unsigned>((kSetupHoldTriggerMs - heldMs + 999) / 1000);
    if (seconds != shownSeconds) {
        lv_label_set_text_fmt(setupUi.countdownNumber, "%u", seconds);
        shownSeconds = seconds;
    }
    // 50 steps over 5 s: smooth enough, and each step redraws only a thin slice of the ring.
    const int progress = static_cast<int>((heldMs - kSetupHoldPromptMs) * 50 / (kSetupHoldTriggerMs - kSetupHoldPromptMs)) * 2;
    if (progress != shownProgress) {
        lv_arc_set_value(setupUi.countdownArc, progress);
        shownProgress = progress;
    }
}

static void uiTask(void *) {
    SharedUiState localState;
    UiRuntimeState localRuntime;

    for (;;) {
        lv_timer_handler();
        serviceSetupHold();
        if (readSharedUiState(localState)) {
            snapshotUiRuntime(localRuntime);
            updateModeUi(localState, localRuntime);
            serviceScriptedDemo();
            serviceWebDemo();
            serviceCarousels();
            serviceScreenshot();
            serviceFpsProbe();

            UiMode autoMode = determineAutoMode(localState);
            if (currentMode != UiMode::EventPopup && autoMode != currentMode) {
                loadMode(autoMode, LV_SCR_LOAD_ANIM_FADE_ON);
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

// --- Web UI hooks (declared in web_server.h) ----------------------------------------------------------------

bool appGetMiniLeagues(WebLeagueInfo *out, size_t capacity, size_t &count) {
    count = 0;
    if (!idleScreensMutex || xSemaphoreTake(idleScreensMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    // During a web demo the screens show demo leagues; the settings page still lists the real ones.
    const IdleScreensData &data = demoIdleOverridden ? *demoRealIdle : *idleScreensData;
    const bool hasLeagues = data.hasLeagues;
    for (size_t i = 0; hasLeagues && i < data.leagueCount && count < capacity; ++i, ++count) {
        out[count].id = data.leagues[i].id;
        strlcpy(out[count].name, data.leagues[i].name, sizeof(out[count].name));
    }
    xSemaphoreGive(idleScreensMutex);
    return hasLeagues;
}

void appRequestFplPoll() {
    fplPollRequested = true;
}

void appShowStatus(const char *text, uint32_t colorHex) {
    setSharedStatus(text, colorHex);
}

// WiFi attempts (15 s each) before falling back to setup mode, if WiFi has never connected since boot.
static constexpr int kWifiAttemptsBeforeSetup = 3;
// Setup mode restarts to retry the saved WiFi after this long with no one connected to the access point
// (e.g. the router was just slow to come back after a power cut).
static constexpr uint32_t kSetupModeIdleRestartMs = 10UL * 60UL * 1000UL;

// Runs the WiFi access point and the setup web UI until the user saves new settings (which restarts).
[[noreturn]] static void runSetupMode() {
    deviceConfigRequestSetupMode(false);  // any restart goes back to normal WiFi

    const uint64_t mac = ESP.getEfuseMac();
    char apName[24];
    snprintf(apName, sizeof(apName), "FPL-Buddy-%02X%02X", static_cast<uint8_t>(mac >> 32),
             static_cast<uint8_t>(mac >> 40));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);  // STA as well, so the UI can scan for networks
    WiFi.softAP(apName);
    strlcpy(setupApName, apName, sizeof(setupApName));
    strlcpy(setupApIp, WiFi.softAPIP().toString().c_str(), sizeof(setupApIp));
    setupModeActive = true;  // uiTask shows the instructions
    webServerStart(true);

    char status[48];
    snprintf(status, sizeof(status), "Setup: join WiFi %s", apName);
    setSharedStatus(status, 0x00E5FF);
    Serial.printf("[SETUP] Join WiFi '%s' and open http://%s/\n", apName, WiFi.softAPIP().toString().c_str());

    static DeviceConfig config;
    deviceConfigGet(config);
    uint32_t lastClientMs = millis();
    for (;;) {
        if (WiFi.softAPgetStationNum() > 0 || webServerOtaInProgress()) {
            lastClientMs = millis();
        } else if (config.wifiSsid[0] && millis() - lastClientMs > kSetupModeIdleRestartMs) {
            Serial.println("[SETUP] no clients, restarting to retry WiFi");
            ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Shows FPL data (from a poll, or the cache when a demo ends) on the display.
static void publishSnapshotToUi(const TeamSnapshot &snapshot) {
    if (snapshot.hasGwState) {
        char gwStateBuf[48];
        snprintf(gwStateBuf, sizeof(gwStateBuf), "GW live: %s | next: %d", snapshot.isLiveGw ? "yes" : "no",
                 snapshot.nextGw);
        setSharedGwStateText(gwStateBuf);
    } else {
        setSharedGwStateText("GW live: ? | next: --");
    }
    updateSharedSquadFromPicks(snapshot.picks, snapshot.pickCount);
    setSharedGwPoints(snapshot.gwPoints);
    setSharedRankData(snapshot.overallRank, snapshot.rankDiff, snapshot.hasRankData);
    setSharedGameweekContext(snapshot.isLiveGw, snapshot.currentGw, snapshot.nextGw, snapshot.nextGw > 0,
                             snapshot.deadlineUtc, snapshot.hasDeadline);
    ledRingSetLiveState(snapshot.isLiveGw, snapshot.matchInPlay);
    setSharedTotalPoints(snapshot.overallPoints, true);
}

static void fplTask(void *) {
    lastPollMs = 0;
    lastWifiRetryMs = 0;
    uint32_t lastSuccessMs = 0;
    uint32_t pollDelayMs = 0;
    uint32_t lastSuccessPollDelayMs = FPL_POLL_INTERVAL_MS;
    bool inDemo = false;
    static TeamSnapshot snapshot;  // static: keeps ~3 KB off the task stack
    bool wifiEverConnected = false;
    int wifiFailures = 0;

    {
        static DeviceConfig config;
        deviceConfigGet(config);
        if (deviceConfigSetupModeRequested() || !config.wifiSsid[0]) {
            runSetupMode();
        }
    }
    setSharedStatus("Connecting WiFi...", 0xFFCC66);

    for (;;) {
        const uint32_t now = millis();

        if (isScriptedDemoActive() || webServerOtaInProgress()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // The demo owns the display (and publishes its own status) until it stops.
        if (isDemoModeEnabled()) {
            inDemo = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (inDemo) {
            // The demo just ended: show the cached FPL data again straight away, then refresh it.
            inDemo = false;
            if (xSemaphoreTake(fplDataMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (fplCache->hasEntry && !isDemoModeEnabled()) {
                    fillSnapshotFromCache(*fplCache, snapshot);
                    publishIdleScreensFromCache(*fplCache);
                    publishSnapshotToUi(snapshot);
                    setSharedFreshness(false, lastSuccessMs);
                }
                xSemaphoreGive(fplDataMutex);
            }
            fplPollRequested = true;
        }

        if (WiFi.status() != WL_CONNECTED) {
            if (lastWifiRetryMs == 0 || now - lastWifiRetryMs >= 10000) {
                lastWifiRetryMs = now;
                setSharedStatus("Reconnecting WiFi...", 0xFFCC66);
                if (connectWiFi()) {
                    wifiEverConnected = true;
                    wifiFailures = 0;
                    webServerStart(false);
                    ensureUkTimeConfigured();
                    char status[48];
                    snprintf(status, sizeof(status), "WiFi: %s", WiFi.localIP().toString().c_str());
                    setSharedStatus(status, 0x38D39F);
                    pollDelayMs = 0;  // refresh as soon as we are back online
                } else if (!wifiEverConnected && ++wifiFailures >= kWifiAttemptsBeforeSetup) {
                    Serial.println("WiFi never connected since boot, starting setup mode");
                    runSetupMode();
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

            if (xSemaphoreTake(fplDataMutex, portMAX_DELAY) == pdTRUE) {
                // The display is updated while holding the cache: a web demo that starts mid-poll waits for the
                // cache before taking over, so the poll can never overwrite the demo afterwards.
                if (!isDemoModeEnabled()) {
                    setSharedStatus("Fetching FPL points...", 0xFFCC66);
                }
                FplPollResult result;
                const bool ok = refreshFplData(result);
                if (ok) {
                    pollDelayMs = result.nextPollDelayMs;
                    lastSuccessPollDelayMs = pollDelayMs;
                    lastSuccessMs = now;
                } else {
                    pollDelayMs = FPL_POLL_INTERVAL_MS;
                }

                if (ok && !isDemoModeEnabled()) {
                    fillSnapshotFromCache(*fplCache, snapshot);
                    publishIdleScreensFromCache(*fplCache);
                    if (snapshot.hasGwState) {
                        Serial.printf("GW state: live=%s next=%d\n", snapshot.isLiveGw ? "yes" : "no", snapshot.nextGw);
                    }
                    if (kUseServerEventBreakdown) {
                        detectAndNotifyPointChangesFromBreakdown(snapshot.currentGw, snapshot.picks, snapshot.pickCount);
                    } else {
                        detectAndNotifyPointChanges(snapshot.currentGw, snapshot.picks, snapshot.pickCount);
                    }
                    if (result.liveFetched) {
                        printTeamSnapshot(snapshot);
                    }
                    publishSnapshotToUi(snapshot);
                    setSharedFreshness(false, lastSuccessMs);
                    setSharedStatus("FPL updated", 0x38D39F);
                    Serial.printf("FPL GW points: %d\n", snapshot.gwPoints);
                } else if (!ok && !isDemoModeEnabled()) {
                    // Idle polls are minutes apart, so "stale" is measured against the interval we expected.
                    const bool stale = (lastSuccessMs == 0) || ((now - lastSuccessMs) > lastSuccessPollDelayMs + 300000U);
                    setSharedFreshness(stale, lastSuccessMs);
                    setSharedStatus("FPL fetch failed", 0xFF5A5A);
                }
                xSemaphoreGive(fplDataMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Pushes the LED ring settings to the ring whenever they change (web UI or serial).
static void applyLedRingSettingsIfChanged(uint32_t &knownVersion) {
    const uint32_t version = displaySettingsVersion();
    if (version == 0 || version == knownVersion) {
        return;
    }
    static DisplaySettings settings;  // static: keeps it off the LED task stack; only this task touches it
    displaySettingsGet(settings);
    ledRingApplySettings(settings.ledRankBrightnessPct, settings.ledDeadlineBrightnessPct,
                         settings.ledNotificationBrightnessPct, settings.ledRankOnlyLiveGw,
                         settings.ledRankOnlyMatchLive);
    knownVersion = version;
}

static void ledTask(void *) {
    uint32_t settingsVersion = 0;
    uint32_t lastSettingsCheckMs = 0;
    for (;;) {
        const uint32_t now = millis();
        if (settingsVersion == 0 || now - lastSettingsCheckMs >= 250) {
            lastSettingsCheckMs = now;
            applyLedRingSettingsIfChanged(settingsVersion);
        }
        ledRingTick(now);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    powerInit();
    Serial.begin(115200);
    delay(500);

    kitImageBuffer = static_cast<uint8_t *>(heap_caps_calloc(1, kKitRgb565Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    fplCache = psramNew<FplDataCache>();
    idleScreensData = psramNew<IdleScreensData>();
    idleUiData = psramNew<IdleScreensData>();
    serialIdleData = psramNew<IdleScreensData>();
    demoRealIdle = psramNew<IdleScreensData>();
    if (!kitImageBuffer || !fplCache || !idleScreensData || !idleUiData || !serialIdleData || !demoRealIdle) {
        Serial.println("PSRAM allocation failed");
        while (true) {
            delay(1000);
        }
    }

    Serial.println("\\n=== Waveshare ESP32-S3-Touch-LCD-1.46B: FPL Buddy ===");
    powerLogState();
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
    lvglBuf2 = static_cast<uint8_t *>(heap_caps_malloc(kLvglBufPixels * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (!lvglBuf || !lvglBuf2) {
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
    // The panel takes big-endian RGB565. Rendering straight into that order saves a byte-swap pass per band,
    // which otherwise sits between one band's transfer finishing and the next one starting.
    lv_display_set_color_format(lvglDisp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(lvglDisp, lvglBuf, lvglBuf2, kLvglBufPixels * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(lvglDisp, lvglRounderCb, LV_EVENT_INVALIDATE_AREA, nullptr);
    display.setTransferDoneCallback(lvglTransferDone, lvglDisp);
    lv_display_add_event_cb(lvglDisp, lvglFrameTimingCb, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(lvglDisp, lvglFrameTimingCb, LV_EVENT_REFR_READY, nullptr);

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
    deviceConfigInit();
    WiFi.persistent(false);  // credentials live in device_config; stop the WiFi driver keeping its own copy
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
    ledRingSetLiveState(false, false);
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
