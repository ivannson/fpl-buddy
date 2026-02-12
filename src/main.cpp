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
#include <time.h>
#include <cstring>

#include "fpl_config.h"
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

static SemaphoreHandle_t uiRuntimeMutex = nullptr;
static UiRuntimeState uiRuntimeState;
static uint8_t kitImageBuffer[kKitRgb565Bytes];
static lv_image_dsc_t kitImageDsc;

struct UiWidgets {
    lv_obj_t *screenIdle = nullptr;
    lv_obj_t *screenDeadline = nullptr;
    lv_obj_t *screenFinalHour = nullptr;
    lv_obj_t *screenLive = nullptr;
    lv_obj_t *screenPopup = nullptr;
    lv_obj_t *screenEvents = nullptr;
    lv_obj_t *screenSquad = nullptr;

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
static void handleSerialCommandLine(char *line);
static void processSerialInput();

static void setupHttpDefaults(HTTPClient &http) {
    http.setConnectTimeout(15000);
    http.setTimeout(30000);
    http.useHTTP10(true);
    http.setUserAgent("fpl-buddy/1.0");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
}

enum class JsonReadMode {
    Stream,
    StringBody
};

static bool getJsonDocument(const String &url, DynamicJsonDocument &doc, JsonDocument *filter = nullptr,
                            JsonReadMode mode = JsonReadMode::Stream) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    for (int attempt = 1; attempt <= 2; ++attempt) {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        setupHttpDefaults(http);

        if (!http.begin(client, url)) {
            return false;
        }

        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("GET failed [%s], HTTP %d\n", url.c_str(), code);
            http.end();
            return false;
        }

        DeserializationError err;
        if (mode == JsonReadMode::StringBody) {
            const String payload = http.getString();
            if (payload.length() == 0) {
                Serial.printf("Empty HTTP payload [%s], attempt %d/2\n", url.c_str(), attempt);
                http.end();
                if (attempt == 1) {
                    delay(200);
                    continue;
                }
                return false;
            }
            if (filter) {
                err = deserializeJson(doc, payload, DeserializationOption::Filter(*filter));
            } else {
                err = deserializeJson(doc, payload);
            }

            if (err) {
                const size_t previewLen = payload.length() < 200 ? payload.length() : 200;
                Serial.printf("JSON parse error [%s] attempt %d/2: %s\n", url.c_str(), attempt, err.c_str());
                Serial.printf("Payload bytes: %u | preview: %.*s\n", static_cast<unsigned>(payload.length()),
                              static_cast<int>(previewLen), payload.c_str());
            }
        } else {
            if (filter) {
                err = deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(*filter));
            } else {
                err = deserializeJson(doc, *http.getStreamPtr());
            }

            if (err) {
                Serial.printf("JSON parse error [%s] attempt %d/2: %s\n", url.c_str(), attempt, err.c_str());
            }
        }
        http.end();

        if (!err) {
            return true;
        }

        // transient short reads are common on embedded HTTPS; retry once
        if (attempt == 1 &&
            (err == DeserializationError::IncompleteInput || err == DeserializationError::EmptyInput)) {
            delay(200);
            continue;
        }

        return false;
    }

    return false;
}

static bool fetchUrlToPsramBuffer(const String &url, char *&bufferOut, size_t &lengthOut, size_t maxBytes) {
    bufferOut = nullptr;
    lengthOut = 0;

    for (int attempt = 1; attempt <= 2; ++attempt) {
        WiFiClientSecure client;
        client.setInsecure();

        HTTPClient http;
        setupHttpDefaults(http);
        if (!http.begin(client, url)) {
            return false;
        }

        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("GET failed [%s], HTTP %d\n", url.c_str(), code);
            http.end();
            return false;
        }

        int contentLen = http.getSize();
        if (contentLen < 0) {
            contentLen = 65536;
        }

        size_t capacity = static_cast<size_t>(contentLen) + 1;
        if (capacity > maxBytes + 1) {
            capacity = maxBytes + 1;
        }
        if (capacity < 4096) {
            capacity = 4096;
        }

        char *buf = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buf) {
            Serial.printf("PSRAM alloc failed [%s]\n", url.c_str());
            http.end();
            return false;
        }

        size_t len = 0;
        uint8_t chunk[1024];
        WiFiClient *stream = http.getStreamPtr();

        while (http.connected() || stream->available()) {
            const int avail = stream->available();
            if (avail <= 0) {
                delay(1);
                continue;
            }

            const size_t want = static_cast<size_t>(avail > static_cast<int>(sizeof(chunk)) ? sizeof(chunk) : avail);
            const int got = stream->readBytes(chunk, want);
            if (got <= 0) {
                continue;
            }

            if (len + static_cast<size_t>(got) + 1 > capacity) {
                size_t newCapacity = capacity * 2;
                if (newCapacity < len + static_cast<size_t>(got) + 1) {
                    newCapacity = len + static_cast<size_t>(got) + 1;
                }
                if (newCapacity > maxBytes + 1) {
                    newCapacity = maxBytes + 1;
                }

                if (newCapacity <= capacity) {
                    Serial.printf("Payload too large [%s] (> %u bytes)\n", url.c_str(), static_cast<unsigned>(maxBytes));
                    heap_caps_free(buf);
                    http.end();
                    return false;
                }

                char *grown = static_cast<char *>(heap_caps_realloc(buf, newCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!grown) {
                    Serial.printf("PSRAM realloc failed [%s]\n", url.c_str());
                    heap_caps_free(buf);
                    http.end();
                    return false;
                }
                buf = grown;
                capacity = newCapacity;
            }

            memcpy(buf + len, chunk, static_cast<size_t>(got));
            len += static_cast<size_t>(got);
        }
        http.end();

        if (len == 0) {
            Serial.printf("Empty HTTP payload [%s], attempt %d/2\n", url.c_str(), attempt);
            heap_caps_free(buf);
            if (attempt == 1) {
                delay(200);
                continue;
            }
            return false;
        }

        buf[len] = '\0';
        bufferOut = buf;
        lengthOut = len;
        Serial.printf("PSRAM payload [%s]: %u bytes\n", url.c_str(), static_cast<unsigned>(len));
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

        DeserializationError err;
        if (filter) {
            err = deserializeJson(doc, payload, payloadLen, DeserializationOption::Filter(*filter));
        } else {
            err = deserializeJson(doc, payload, payloadLen);
        }
        heap_caps_free(payload);

        if (!err) {
            return true;
        }

        Serial.printf("JSON parse error [%s] from PSRAM attempt %d/2: %s\n", url.c_str(), attempt, err.c_str());
        if (attempt == 1 &&
            (err == DeserializationError::IncompleteInput || err == DeserializationError::EmptyInput)) {
            delay(200);
            continue;
        }
        return false;
    }
    return false;
}

static bool fetchEntrySummary(int &currentGwOut, int &overallRankOut, int &overallPointsOut) {
    DynamicJsonDocument filter(256);
    filter["current_event"] = true;
    filter["summary_overall_rank"] = true;
    filter["summary_overall_points"] = true;

    DynamicJsonDocument doc(1024);
    String url = "https://fantasy.premierleague.com/api/entry/";
    url += String(FPL_ENTRY_ID);
    url += "/";

    if (!getJsonDocument(url, doc, &filter, JsonReadMode::StringBody)) {
        return false;
    }

    if (!doc["current_event"].is<int>()) {
        Serial.println("entry response missing current_event");
        return false;
    }

    currentGwOut = doc["current_event"].as<int>();
    overallRankOut = doc["summary_overall_rank"] | 0;
    overallPointsOut = doc["summary_overall_points"] | 0;
    return true;
}

static bool fetchPreviousOverallRank(int currentGw, int &prevRankOut) {
    DynamicJsonDocument filter(512);
    JsonArray currentFilter = filter.createNestedArray("current");
    JsonObject currentEventFilter = currentFilter.createNestedObject();
    currentEventFilter["event"] = true;
    currentEventFilter["overall_rank"] = true;

    DynamicJsonDocument doc(16384);
    String url = "https://fantasy.premierleague.com/api/entry/";
    url += String(FPL_ENTRY_ID);
    url += "/history/";

    if (!getJsonDocument(url, doc, &filter, JsonReadMode::StringBody)) {
        return false;
    }

    JsonArray current = doc["current"].as<JsonArray>();
    if (current.isNull() || current.size() == 0) {
        return false;
    }

    int bestEvent = -1;
    int bestRank = 0;
    const int targetEvent = currentGw - 1;
    for (JsonObject e : current) {
        const int ev = e["event"] | 0;
        const int rank = e["overall_rank"] | 0;
        if (rank <= 0) {
            continue;
        }
        if (targetEvent > 0 && ev == targetEvent) {
            prevRankOut = rank;
            return true;
        }
        if (ev < currentGw && ev > bestEvent) {
            bestEvent = ev;
            bestRank = rank;
        }
    }

    if (bestEvent > 0 && bestRank > 0) {
        prevRankOut = bestRank;
        return true;
    }
    return false;
}

static bool fetchGameweekState(bool &isLiveOut, int &nextGwOut, bool &hasDeadlineOut, time_t &deadlineOut) {
    DynamicJsonDocument filter(512);
    JsonArray eventsFilter = filter.createNestedArray("events");
    JsonObject eventFilter = eventsFilter.createNestedObject();
    eventFilter["id"] = true;
    eventFilter["is_current"] = true;
    eventFilter["is_next"] = true;
    eventFilter["finished"] = true;
    eventFilter["deadline_time"] = true;
    eventFilter["deadline_time_epoch"] = true;

    DynamicJsonDocument doc(8192);
    const String url = "https://fantasy.premierleague.com/api/bootstrap-static/";
    if (!getJsonDocumentFromPsramUrl(url, doc, FPL_BOOTSTRAP_PSRAM_MAX_BYTES, &filter)) {
        return false;
    }

    JsonArray events = doc["events"].as<JsonArray>();
    if (events.isNull()) {
        Serial.println("bootstrap response missing events");
        return false;
    }

    bool foundCurrent = false;
    bool foundNext = false;
    bool currentFinished = false;
    int nextGw = 0;
    bool hasDeadline = false;
    time_t parsedDeadline = 0;

    for (JsonObject e : events) {
        if (e["is_current"] | false) {
            foundCurrent = true;
            currentFinished = e["finished"] | false;
        }
        if (e["is_next"] | false) {
            foundNext = true;
            nextGw = e["id"] | 0;
            const char *deadlineIso = e["deadline_time"] | nullptr;
            if (parseIsoUtcToEpoch(deadlineIso, parsedDeadline)) {
                hasDeadline = true;
            } else {
                const int64_t epoch = e["deadline_time_epoch"] | 0;
                if (epoch > 0) {
                    parsedDeadline = static_cast<time_t>(epoch);
                    hasDeadline = true;
                    Serial.printf("Using deadline_time_epoch fallback for GW%d: %lld\n", nextGw,
                                  static_cast<long long>(epoch));
                } else {
                    Serial.printf("Failed to parse deadline_time for GW%d: %s\n", nextGw,
                                  deadlineIso ? deadlineIso : "null");
                }
            }
        }
    }

    if (!foundCurrent && !foundNext) {
        return false;
    }

    // Proxy for live state from bootstrap event flags.
    isLiveOut = foundCurrent && !currentFinished;
    nextGwOut = foundNext ? nextGw : 0;
    hasDeadlineOut = hasDeadline;
    deadlineOut = parsedDeadline;
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
    String url = "https://fantasy.premierleague.com/api/entry/";
    url += String(FPL_ENTRY_ID);
    url += "/event/";
    url += String(gw);
    url += "/picks/";

    if (!getJsonDocument(url, doc, &filter, JsonReadMode::StringBody)) {
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
        pick.elementId = p["element"] | 0;
        pick.squadPosition = p["position"] | 0;
        pick.multiplier = p["multiplier"] | 0;
        pick.isCaptain = p["is_captain"] | false;
        pick.isViceCaptain = p["is_vice_captain"] | false;
    }

    return pickCountOut > 0;
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
        {"manchester_city", "man_city"},
        {"manchester_utd", "man_utd"},
        {"manchester_united", "man_utd"},
        {"newcastle_utd", "newcastle"},
        {"newcastle_united", "newcastle"},
        {"nott_m_forest", "nottingham_forest"},
        {"nottm_forest", "nottingham_forest"},
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

static bool fetchPlayerMetaForPicks(TeamPick *picks, size_t pickCount) {
    DynamicJsonDocument filter(1152);
    JsonArray elementsFilter = filter.createNestedArray("elements");
    JsonObject elementFilter = elementsFilter.createNestedObject();
    elementFilter["id"] = true;
    elementFilter["web_name"] = true;
    elementFilter["element_type"] = true;
    elementFilter["team"] = true;

    JsonArray typesFilter = filter.createNestedArray("element_types");
    JsonObject typeFilter = typesFilter.createNestedObject();
    typeFilter["id"] = true;
    typeFilter["singular_name_short"] = true;

    JsonArray teamsFilter = filter.createNestedArray("teams");
    JsonObject teamFilter = teamsFilter.createNestedObject();
    teamFilter["id"] = true;
    teamFilter["name"] = true;

    DynamicJsonDocument doc(90000);
    const String url = "https://fantasy.premierleague.com/api/bootstrap-static/";
    if (!getJsonDocumentFromPsramUrl(url, doc, FPL_BOOTSTRAP_PSRAM_MAX_BYTES, &filter)) {
        return false;
    }

    struct TypeName {
        int id;
        const char *name;
    };
    TypeName typeNames[8];
    size_t typeCount = 0;
    struct TeamShort {
        int id = 0;
        char slug[24] = "";
    };
    TeamShort teamNames[24];
    size_t teamCount = 0;

    JsonArray types = doc["element_types"].as<JsonArray>();
    for (JsonObject t : types) {
        if (typeCount >= (sizeof(typeNames) / sizeof(typeNames[0]))) {
            break;
        }
        typeNames[typeCount].id = t["id"] | 0;
        typeNames[typeCount].name = t["singular_name_short"] | "?";
        ++typeCount;
    }

    JsonArray teams = doc["teams"].as<JsonArray>();
    for (JsonObject t : teams) {
        if (teamCount >= (sizeof(teamNames) / sizeof(teamNames[0]))) {
            break;
        }
        teamNames[teamCount].id = t["id"] | 0;
        slugifyTeamName(t["name"] | "", teamNames[teamCount].slug, sizeof(teamNames[teamCount].slug));
        normalizeKitTeamSlug(teamNames[teamCount].slug, sizeof(teamNames[teamCount].slug));
        ++teamCount;
    }

    JsonArray elements = doc["elements"].as<JsonArray>();
    for (JsonObject e : elements) {
        const int id = e["id"] | 0;
        for (size_t i = 0; i < pickCount; ++i) {
            if (picks[i].elementId != id) {
                continue;
            }
            picks[i].playerName = e["web_name"] | "unknown";
            const int typeId = e["element_type"] | 0;
            picks[i].elementType = typeId;
            picks[i].teamId = e["team"] | 0;
            picks[i].teamShortName[0] = '\0';
            picks[i].positionName = "?";
            for (size_t j = 0; j < typeCount; ++j) {
                if (typeNames[j].id == typeId) {
                    picks[i].positionName = typeNames[j].name;
                    break;
                }
            }
            for (size_t j = 0; j < teamCount; ++j) {
                if (teamNames[j].id == picks[i].teamId) {
                    strlcpy(picks[i].teamShortName, teamNames[j].slug, sizeof(picks[i].teamShortName));
                    break;
                }
            }
            break;
        }
    }

    return true;
}
#endif

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

static bool fetchTeamSnapshot(TeamSnapshot &out) {
    int currentGw = 0;
    int overallRank = 0;
    int overallPoints = 0;
    if (!fetchEntrySummary(currentGw, overallRank, overallPoints)) {
        return false;
    }

    size_t pickCount = 0;
    String activeChip;
    if (!fetchPicksForGw(currentGw, out.picks, 16, pickCount, activeChip)) {
        return false;
    }
    if (!fetchLivePointsForPicks(currentGw, out.picks, pickCount)) {
        return false;
    }

    bool hasPlayerMeta = false;
#if FPL_ENABLE_NAME_LOOKUP
    hasPlayerMeta = fetchPlayerMetaForPicks(out.picks, pickCount);
#endif

    out.currentGw = currentGw;
    out.overallRank = overallRank;
    out.overallPoints = overallPoints;
    out.pickCount = pickCount;
    out.activeChip = activeChip;
    out.hasPlayerMeta = hasPlayerMeta;
    out.gwPoints = computeGwPointsFromPicks(out.picks, out.pickCount);
    return true;
}

static bool fetchAndPrintTeamSnapshot(int &gwPointsOut, int *currentGwOut = nullptr, int *overallPointsOut = nullptr) {
    TeamSnapshot snapshot;
    if (!fetchTeamSnapshot(snapshot)) {
        return false;
    }

    updateSharedSquadFromPicks(snapshot.picks, snapshot.pickCount);

    if (kUseServerEventBreakdown) {
        detectAndNotifyPointChangesFromBreakdown(snapshot.currentGw, snapshot.picks, snapshot.pickCount);
    } else {
        detectAndNotifyPointChanges(snapshot.currentGw, snapshot.picks, snapshot.pickCount);
    }
    gwPointsOut = snapshot.gwPoints;

    Serial.println("\n=== FPL Team Snapshot ===");
    Serial.printf("Entry ID: %d | GW: %d | GW points: %d\n", FPL_ENTRY_ID, snapshot.currentGw, gwPointsOut);
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
            if (showBonusState) {
                Serial.printf(" | bonus:%d(%s)", p.live.bonus, bonusState);
            }
            Serial.println();
            Serial.printf("       breakdown: %s\n", breakdown);
        } else {
            Serial.printf("  [%2d] %-5s | curr:%2d | element:%4d | mult:%d%s%s | eff:%2d", p.squadPosition, slot,
                          currPoints, p.elementId, p.multiplier, p.isCaptain ? " C" : "",
                          p.isViceCaptain ? " VC" : "", effectivePoints);
            if (showBonusState) {
                Serial.printf(" | bonus:%d(%s)", p.live.bonus, bonusState);
            }
            Serial.println();
            Serial.printf("       breakdown: %s\n", breakdown);
        }
    }
    Serial.println("=========================\n");

    if (currentGwOut) {
        *currentGwOut = snapshot.currentGw;
    }
    if (overallPointsOut) {
        *overallPointsOut = snapshot.overallPoints;
    }

    return true;
}

static bool fetchRankDelta(int &overallRankOut, int &rankDiffOut) {
    int currentGw = 0;
    int overallRank = 0;
    int overallPoints = 0;
    if (!fetchEntrySummary(currentGw, overallRank, overallPoints)) {
        return false;
    }
    if (overallRank <= 0) {
        return false;
    }

    int previousRank = 0;
    if (!fetchPreviousOverallRank(currentGw, previousRank) || previousRank <= 0) {
        return false;
    }

    // Positive means improvement (rank number got smaller).
    overallRankOut = overallRank;
    rankDiffOut = previousRank - overallRank;
    return true;
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
static uint32_t lastTickerRotateMs = 0;
static uint32_t lastDeadlineBlinkMs = 0;
static bool deadlineColonVisible = true;
static size_t tickerEventIndex = 0;
static uint32_t holdStartMs = 0;
static bool holdTriggered = false;
static uint32_t renderedEventsVersion = 0;
static uint32_t renderedSquadVersion = 0;

static lv_obj_t *modeToScreen(UiMode mode) {
    switch (mode) {
        case UiMode::Idle: return ui.screenIdle;
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

    ui.deadlineLabel = createLabel(ui.screenDeadline, &lv_font_montserrat_28, kColorAccentAmber, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(ui.deadlineLabel, "DEADLINE");
    lv_obj_align(ui.deadlineLabel, LV_ALIGN_CENTER, 0, -110);
    ui.deadlineCountdown = createLabel(ui.screenDeadline, kFontHero, kColorTextPrimary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.deadlineCountdown, LV_ALIGN_CENTER, 0, -20);
    ui.deadlineMeta = createLabel(ui.screenDeadline, &lv_font_montserrat_22, kColorTextSecondary, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(ui.deadlineMeta, LV_ALIGN_CENTER, 0, 84);

    ui.finalArc = lv_arc_create(ui.screenFinalHour);
    lv_obj_set_size(ui.finalArc, 340, 340);
    lv_obj_center(ui.finalArc);
    lv_arc_set_range(ui.finalArc, 0, 3600);
    lv_obj_set_style_arc_width(ui.finalArc, 30, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.finalArc, 30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.finalArc, lv_color_hex(0x242424), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.finalArc, lv_color_hex(kColorAccentAmber), LV_PART_INDICATOR);
    lv_obj_remove_style(ui.finalArc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(ui.finalArc, LV_OBJ_FLAG_CLICKABLE);
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
        return;
    }

    if (strcmp(tokens[0], "demo") == 0) {
        if (tokenCount < 2 || strcmp(tokens[1], "help") == 0) {
            printDemoHelp();
            return;
        }

        if (strcmp(tokens[1], "seed") == 0) {
            if (WiFi.status() != WL_CONNECTED && !connectWiFi()) {
                Serial.println("[DEMO] Seed failed: WiFi not connected");
                return;
            }
            ensureUkTimeConfigured();

            TeamSnapshot snapshot;
            if (!fetchTeamSnapshot(snapshot)) {
                Serial.println("[DEMO] Seed failed: could not fetch team snapshot");
                return;
            }

            bool isLive = false;
            int nextGw = 0;
            bool hasDeadline = false;
            time_t deadlineUtc = 0;
            fetchGameweekState(isLive, nextGw, hasDeadline, deadlineUtc);

            int rank = snapshot.overallRank;
            int rankDiff = 0;
            const bool hasRankData = fetchRankDelta(rank, rankDiff);

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

    if (ui.finalArc && ui.finalCountdown && state.hasNextDeadline) {
        const time_t now = time(nullptr);
        int64_t sec = now > 100000 ? (static_cast<int64_t>(state.nextDeadlineUtc) - static_cast<int64_t>(now)) : 0;
        if (sec < 0) {
            sec = 0;
        }
        if (sec > 3600) {
            sec = 3600;
        }
        lv_arc_set_value(ui.finalArc, static_cast<int>(sec));
        if (sec < 900) {
            lv_obj_set_style_arc_color(ui.finalArc, lv_color_hex(kColorAccentRed), LV_PART_INDICATOR);
        } else {
            lv_obj_set_style_arc_color(ui.finalArc, lv_color_hex(kColorAccentAmber), LV_PART_INDICATOR);
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
    popupHideAtMs = millis() + 4000U;
}

static void uiTask(void *) {
    SharedUiState localState;
    UiRuntimeState localRuntime;

    for (;;) {
        lv_timer_handler();
        if (readSharedUiState(localState)) {
            snapshotUiRuntime(localRuntime);
            updateModeUi(localState, localRuntime);

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
    bool demoModeAnnounced = false;
    setSharedStatus("Connecting WiFi...", 0xFFCC66);

    for (;;) {
        const uint32_t now = millis();

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
                } else {
                    setSharedStatus("WiFi not connected", 0xFF5A5A);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (lastPollMs == 0 || now - lastPollMs >= FPL_POLL_INTERVAL_MS) {
            lastPollMs = now;
            setSharedStatus("Fetching FPL points...", 0xFFCC66);

            bool isLive = false;
            int nextGw = 0;
            bool hasDeadline = false;
            time_t deadlineUtc = 0;
            if (fetchGameweekState(isLive, nextGw, hasDeadline, deadlineUtc)) {
                char gwStateBuf[48];
                snprintf(gwStateBuf, sizeof(gwStateBuf), "GW live: %s | next: %d", isLive ? "yes" : "no", nextGw);
                setSharedGwStateText(gwStateBuf);
                Serial.printf("GW state: live=%s next=%d\n", isLive ? "yes" : "no", nextGw);
            } else {
                setSharedGwStateText("GW live: ? | next: --");
            }

            int gwPoints = 0;
            int currentGw = 0;
            int totalPoints = 0;
            if (fetchAndPrintTeamSnapshot(gwPoints, &currentGw, &totalPoints)) {
                setSharedGwPoints(gwPoints);
                int overallRank = 0;
                int rankDiff = 0;
                const bool hasRank = fetchRankDelta(overallRank, rankDiff);
                setSharedRankData(overallRank, rankDiff, hasRank);
                setSharedGameweekContext(isLive, currentGw, nextGw, nextGw > 0, deadlineUtc, hasDeadline);
                setSharedTotalPoints(totalPoints, true);
                lastSuccessMs = now;
                setSharedFreshness(false, lastSuccessMs);
                setSharedStatus("FPL updated", 0x38D39F);
                Serial.printf("FPL GW points: %d\n", gwPoints);
            } else {
                const bool stale = (lastSuccessMs == 0) || ((now - lastSuccessMs) > 300000U);
                setSharedFreshness(stale, lastSuccessMs);
                setSharedStatus("FPL fetch failed", 0xFF5A5A);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\\n=== Waveshare ESP32-S3-Touch-LCD-1.46B: FPL Buddy ===");

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

    if (!uiTaskHandle || !fplTaskHandle) {
        Serial.println("Failed to create worker tasks");
        while (true) {
            delay(1000);
        }
    }
    Serial.println("Worker tasks started");
    Serial.println("Type `demo help` in serial monitor for manual demo controls");
}

void loop() {
    processSerialInput();
    vTaskDelay(pdMS_TO_TICKS(20));
}
