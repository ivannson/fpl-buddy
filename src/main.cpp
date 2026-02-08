#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPD2010.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <time.h>

#include "fpl_config.h"
#include "wifi_config.h"

static SPD2010Display display;
static SPD2010Touch touch;

static constexpr uint16_t kDisplayWidth = SPD2010_WIDTH;
static constexpr uint16_t kDisplayHeight = SPD2010_HEIGHT;
static constexpr size_t kLvglBufPixels = kDisplayWidth * 40;

static lv_display_t *lvglDisp = nullptr;
static uint8_t *lvglBuf = nullptr;

// UI objects
static lv_obj_t *arcProgress = nullptr;
static lv_obj_t *labelStatus = nullptr;
static lv_obj_t *labelPoints = nullptr;
static lv_obj_t *labelGwState = nullptr;
static lv_obj_t *labelUkTime = nullptr;
static lv_obj_t *labelDeadline = nullptr;

static uint32_t lastPollMs = 0;
static uint32_t lastWifiRetryMs = 0;
static TaskHandle_t uiTaskHandle = nullptr;
static TaskHandle_t fplTaskHandle = nullptr;

struct SharedUiState {
    int gwPoints = 0;
    bool hasGwPoints = false;
    uint32_t statusColor = 0xFFFFFF;
    char statusText[48] = "Booting...";
    char gwStateText[48] = "GW live: ? | next: --";
    int nextGw = 0;
    bool hasNextGw = false;
    time_t nextDeadlineUtc = 0;
    bool hasNextDeadline = false;
    uint32_t version = 0;
};

static SharedUiState sharedUiState;
static SemaphoreHandle_t sharedUiMutex = nullptr;
static bool timeConfigured = false;
static bool parseIsoUtcToEpoch(const char *iso, time_t &epochOut);

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
    };

    int elementId = 0;
    int squadPosition = 0;
    int multiplier = 0;
    bool isCaptain = false;
    bool isViceCaptain = false;
    int elementType = 0;  // 1=GK, 2=DEF, 3=MID, 4=FWD
    LiveStats live;
    String playerName;
    String positionName;
};

struct LastPickState {
    bool valid = false;
    int gw = 0;
    int elementId = 0;
    TeamPick::LiveStats live;
};

static LastPickState lastPickStates[16];

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

static bool fetchEntrySummary(int &currentGwOut) {
    DynamicJsonDocument filter(256);
    filter["current_event"] = true;

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
    return true;
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

    DynamicJsonDocument doc(260000);
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
                break;
            }
        }
    }

    return true;
}

#if FPL_ENABLE_NAME_LOOKUP
static bool fetchPlayerMetaForPicks(TeamPick *picks, size_t pickCount) {
    DynamicJsonDocument filter(768);
    JsonArray elementsFilter = filter.createNestedArray("elements");
    JsonObject elementFilter = elementsFilter.createNestedObject();
    elementFilter["id"] = true;
    elementFilter["web_name"] = true;
    elementFilter["element_type"] = true;

    JsonArray typesFilter = filter.createNestedArray("element_types");
    JsonObject typeFilter = typesFilter.createNestedObject();
    typeFilter["id"] = true;
    typeFilter["singular_name_short"] = true;

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

    JsonArray types = doc["element_types"].as<JsonArray>();
    for (JsonObject t : types) {
        if (typeCount >= (sizeof(typeNames) / sizeof(typeNames[0]))) {
            break;
        }
        typeNames[typeCount].id = t["id"] | 0;
        typeNames[typeCount].name = t["singular_name_short"] | "?";
        ++typeCount;
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
            picks[i].positionName = "?";
            for (size_t j = 0; j < typeCount; ++j) {
                if (typeNames[j].id == typeId) {
                    picks[i].positionName = typeNames[j].name;
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

static void notifyEvent(const char *name, int pts, const char *what) {
    Serial.printf("[FPL EVENT] %s %+d pt%s, %s\n", name, pts, (pts == 1 || pts == -1) ? "" : "s", what);
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

        char nameBuf[24];
        const char *name = pickDisplayName(p, nameBuf, sizeof(nameBuf));
        int explained = 0;

        if (prev.minutes < 1 && curr.minutes >= 1) {
            notifyEvent(name, +1, "entered match (1-59 mins)");
            explained += 1;
        }
        if (prev.minutes < 60 && curr.minutes >= 60) {
            notifyEvent(name, +1, "60 mins played");
            explained += 1;
        }

        const int goalDiff = curr.goalsScored - prev.goalsScored;
        if (goalDiff > 0) {
            const int pts = goalPointsForElementType(p.elementType) * goalDiff;
            const char *what = (goalDiff == 1) ? "goal scored" : "goals scored";
            notifyEvent(name, pts, what);
            explained += pts;
        }

        const int assistDiff = curr.assists - prev.assists;
        if (assistDiff > 0) {
            const int pts = 3 * assistDiff;
            const char *what = (assistDiff == 1) ? "assist" : "assists";
            notifyEvent(name, pts, what);
            explained += pts;
        }

        const int csDiff = curr.cleanSheets - prev.cleanSheets;
        if (csDiff > 0) {
            const int pts = cleanSheetPointsForElementType(p.elementType) * csDiff;
            if (pts != 0) {
                notifyEvent(name, pts, "clean sheet");
                explained += pts;
            }
        }

        const int savesThreshold = savesThresholdForElementType(p.elementType);
        if (savesThreshold > 0) {
            const int saveChunksPrev = prev.saves / savesThreshold;
            const int saveChunksCurr = curr.saves / savesThreshold;
            const int chunkDiff = saveChunksCurr - saveChunksPrev;
            if (chunkDiff > 0) {
                notifyEvent(name, chunkDiff, "save points (every 3 saves)");
                explained += chunkDiff;
            }
        }

        const int psDiff = curr.penaltiesSaved - prev.penaltiesSaved;
        if (psDiff > 0) {
            const int pts = 5 * psDiff;
            notifyEvent(name, pts, "penalty saved");
            explained += pts;
        }

        const int dcThreshold = defensiveContributionThresholdForElementType(p.elementType);
        if (dcThreshold > 0) {
            const int dcChunksPrev = prev.defensiveContributions / dcThreshold;
            const int dcChunksCurr = curr.defensiveContributions / dcThreshold;
            const int chunkDiff = dcChunksCurr - dcChunksPrev;
            if (chunkDiff > 0) {
                const int pts = 2 * chunkDiff;
                notifyEvent(name, pts, "defensive contributions threshold");
                explained += pts;
            }
        }

        const int bonusDiff = curr.bonus - prev.bonus;
        if (bonusDiff > 0) {
            notifyEvent(name, bonusDiff, "bonus points");
            explained += bonusDiff;
        }

        if (p.elementType == 1 || p.elementType == 2) {
            const int gcChunksPrev = prev.goalsConceded / 2;
            const int gcChunksCurr = curr.goalsConceded / 2;
            const int gcChunkDiff = gcChunksCurr - gcChunksPrev;
            if (gcChunkDiff > 0) {
                notifyEvent(name, -gcChunkDiff, "goals conceded deduction");
                explained -= gcChunkDiff;
            }
        }

        const int pmDiff = curr.penaltiesMissed - prev.penaltiesMissed;
        if (pmDiff > 0) {
            const int pts = -2 * pmDiff;
            notifyEvent(name, pts, "penalty missed");
            explained += pts;
        }

        const int ycDiff = curr.yellowCards - prev.yellowCards;
        if (ycDiff > 0) {
            const int pts = -ycDiff;
            notifyEvent(name, pts, "yellow card");
            explained += pts;
        }

        const int rcDiff = curr.redCards - prev.redCards;
        if (rcDiff > 0) {
            const int pts = -3 * rcDiff;
            notifyEvent(name, pts, "red card");
            explained += pts;
        }

        const int ogDiff = curr.ownGoals - prev.ownGoals;
        if (ogDiff > 0) {
            const int pts = -2 * ogDiff;
            notifyEvent(name, pts, "own goal");
            explained += pts;
        }

        if (explained != pointDelta) {
            Serial.printf("[FPL EVENT] %s %+d pts total change (unattributed %+d)\n", name, pointDelta,
                          pointDelta - explained);
        }

        state->live = curr;
    }
}

static bool fetchAndPrintTeamSnapshot(int &gwPointsOut) {
    int currentGw = 0;
    if (!fetchEntrySummary(currentGw)) {
        return false;
    }

    TeamPick picks[16];
    size_t pickCount = 0;
    String activeChip;
    if (!fetchPicksForGw(currentGw, picks, 16, pickCount, activeChip)) {
        return false;
    }
    if (!fetchLivePointsForPicks(currentGw, picks, pickCount)) {
        return false;
    }
    bool hasPlayerMeta = false;
#if FPL_ENABLE_NAME_LOOKUP
    hasPlayerMeta = fetchPlayerMetaForPicks(picks, pickCount);
#endif

    detectAndNotifyPointChanges(currentGw, picks, pickCount);

    int adjustedPoints[16];
    bool projectedBonusAdded[16];
    bool bonusIncluded[16];
    int computedGwPoints = 0;
    for (size_t i = 0; i < pickCount; ++i) {
        adjustedPoints[i] =
            adjustedLivePointsWithProjectedBonus(picks[i], projectedBonusAdded[i], bonusIncluded[i]);
        computedGwPoints += adjustedPoints[i] * picks[i].multiplier;
    }
    gwPointsOut = computedGwPoints;

    Serial.println("\n=== FPL Team Snapshot ===");
    Serial.printf("Entry ID: %d | GW: %d | GW points: %d\n", FPL_ENTRY_ID, currentGw, gwPointsOut);
    Serial.printf("Active chip: %s\n", activeChip.c_str());
#if FPL_ENABLE_NAME_LOOKUP
    Serial.printf("Name lookup: %s\n", hasPlayerMeta ? "ok" : "fallback-id-only");
#else
    Serial.println("Name lookup: disabled");
#endif
    Serial.println("Players:");

    for (size_t i = 0; i < pickCount; ++i) {
        TeamPick &p = picks[i];
        const char *slot = (p.squadPosition <= 11) ? "XI" : "BENCH";
        const int currPoints = adjustedPoints[i];
        const int effectivePoints = currPoints * p.multiplier;
        const bool showBonusState = p.live.bonus > 0;
        const char *bonusState = projectedBonusAdded[i] ? "proj" : (bonusIncluded[i] ? "in" : "unk");
        char breakdown[256];
        formatPointsBreakdown(p, projectedBonusAdded[i], bonusIncluded[i], currPoints, breakdown, sizeof(breakdown));
        if (hasPlayerMeta) {
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

static void arcAnimCb(void *var, int32_t value) {
    lv_arc_set_value(static_cast<lv_obj_t *>(var), static_cast<int16_t>(value));
}

static void updateStatus(const char *text, lv_color_t color = lv_color_hex(0xFFFFFF)) {
    if (!labelStatus) {
        return;
    }
    lv_label_set_text(labelStatus, text);
    lv_obj_set_style_text_color(labelStatus, color, LV_PART_MAIN);
}

static void updatePoints(int points) {
    if (!labelPoints) {
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "GW Points\n%d", points);
    lv_label_set_text(labelPoints, buf);
}

static void updateGwStateText(const char *text) {
    if (!labelGwState) {
        return;
    }
    lv_label_set_text(labelGwState, text);
}

static void updateUkTimeText(const char *text) {
    if (!labelUkTime) {
        return;
    }
    lv_label_set_text(labelUkTime, text);
}

static void updateDeadlineText(const char *text) {
    if (!labelDeadline) {
        return;
    }
    lv_label_set_text(labelDeadline, text);
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

static void setSharedGwTiming(int nextGw, bool hasNextGw, time_t deadlineUtc, bool hasDeadline) {
    if (!sharedUiMutex) {
        return;
    }
    if (xSemaphoreTake(sharedUiMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    sharedUiState.nextGw = nextGw;
    sharedUiState.hasNextGw = hasNextGw;
    sharedUiState.nextDeadlineUtc = deadlineUtc;
    sharedUiState.hasNextDeadline = hasDeadline;
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

static void formatUkNow(char *out, size_t outLen) {
    const time_t now = time(nullptr);
    if (now <= 100000) {
        strlcpy(out, "UK time: --", outLen);
        return;
    }
    struct tm localTm;
    localtime_r(&now, &localTm);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%a %d %b %H:%M", &localTm);
    snprintf(out, outLen, "UK: %s", timeBuf);
}

static void formatDeadlineInfo(const SharedUiState &state, char *out, size_t outLen) {
    if (!state.hasNextDeadline || state.nextDeadlineUtc <= 0) {
        strlcpy(out, "Deadline: --\nLeft: --", outLen);
        return;
    }

    struct tm deadlineTm;
    localtime_r(&state.nextDeadlineUtc, &deadlineTm);
    char deadlineBuf[40];
    strftime(deadlineBuf, sizeof(deadlineBuf), "%a %d %b %H:%M", &deadlineTm);

    const time_t now = time(nullptr);
    if (now <= 100000) {
        snprintf(out, outLen, "GW%d DL: %s\nLeft: --", state.hasNextGw ? state.nextGw : 0, deadlineBuf);
        return;
    }

    int64_t diff = static_cast<int64_t>(state.nextDeadlineUtc) - static_cast<int64_t>(now);
    if (diff < 0) {
        diff = 0;
    }

    const int days = static_cast<int>(diff / 86400);
    diff %= 86400;
    const int hours = static_cast<int>(diff / 3600);
    diff %= 3600;
    const int minutes = static_cast<int>(diff / 60);

    snprintf(out, outLen, "GW%d DL: %s\nLeft: %dd %02dh %02dm", state.hasNextGw ? state.nextGw : 0, deadlineBuf,
             days, hours, minutes);
}

static void createUi() {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    const int min_side = (kDisplayWidth < kDisplayHeight) ? kDisplayWidth : kDisplayHeight;
    const int arc_size = min_side - 8;
    const int ring_w = 20;
    const int center_circle = min_side / 5;

    arcProgress = lv_arc_create(screen);
    lv_obj_set_size(arcProgress, arc_size, arc_size);
    lv_obj_center(arcProgress);

    lv_arc_set_rotation(arcProgress, 270);
    lv_arc_set_bg_angles(arcProgress, 0, 360);
    lv_arc_set_value(arcProgress, 0);
    lv_arc_set_range(arcProgress, 0, 100);

    lv_obj_set_style_arc_color(arcProgress, lv_color_hex(0xFFA500), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arcProgress, ring_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arcProgress, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arcProgress, ring_w, LV_PART_MAIN);

    lv_obj_remove_style(arcProgress, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arcProgress, LV_OBJ_FLAG_CLICKABLE);

    labelPoints = lv_label_create(screen);
    lv_label_set_text(labelPoints, "GW Points\n--");
    lv_obj_set_style_text_color(labelPoints, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(labelPoints, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(labelPoints, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(labelPoints, LV_ALIGN_CENTER, 0, center_circle / 2);

    labelStatus = lv_label_create(screen);
    lv_label_set_text(labelStatus, "Booting...");
    lv_obj_set_style_text_color(labelStatus, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_set_style_text_align(labelStatus, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(labelStatus, kDisplayWidth - 40);
    lv_obj_align(labelStatus, LV_ALIGN_CENTER, 0, 156);

    labelGwState = lv_label_create(screen);
    lv_label_set_text(labelGwState, "GW live: ? | next: --");
    lv_obj_set_style_text_color(labelGwState, lv_color_hex(0x8FA1B3), LV_PART_MAIN);
    lv_obj_set_style_text_align(labelGwState, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(labelGwState, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(labelGwState, kDisplayWidth - 40);
    lv_obj_align(labelGwState, LV_ALIGN_CENTER, 0, -106);

    labelUkTime = lv_label_create(screen);
    lv_label_set_text(labelUkTime, "UK: --");
    lv_obj_set_style_text_color(labelUkTime, lv_color_hex(0xB9C6D3), LV_PART_MAIN);
    lv_obj_set_style_text_align(labelUkTime, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(labelUkTime, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(labelUkTime, kDisplayWidth - 40);
    lv_obj_align(labelUkTime, LV_ALIGN_CENTER, 0, -82);

    labelDeadline = lv_label_create(screen);
    lv_label_set_text(labelDeadline, "Deadline: --\nLeft: --");
    lv_obj_set_style_text_color(labelDeadline, lv_color_hex(0xD8DEE5), LV_PART_MAIN);
    lv_obj_set_style_text_align(labelDeadline, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(labelDeadline, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(labelDeadline, kDisplayWidth - 46);
    lv_obj_align(labelDeadline, LV_ALIGN_CENTER, 0, 104);

    lv_anim_t arcAnimation;
    lv_anim_init(&arcAnimation);
    lv_anim_set_var(&arcAnimation, arcProgress);
    lv_anim_set_exec_cb(&arcAnimation, arcAnimCb);
    lv_anim_set_values(&arcAnimation, 0, 100);
    lv_anim_set_duration(&arcAnimation, 5000);
    lv_anim_set_repeat_count(&arcAnimation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&arcAnimation, 0);
    lv_anim_start(&arcAnimation);
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

static void uiTask(void *) {
    SharedUiState localState;
    uint32_t appliedVersion = 0;
    uint32_t lastTimeUiUpdateMs = 0;

    for (;;) {
        lv_timer_handler();

        if (readSharedUiState(localState) && localState.version != appliedVersion) {
            if (localState.hasGwPoints) {
                updatePoints(localState.gwPoints);
            }
            updateStatus(localState.statusText, lv_color_hex(localState.statusColor));
            updateGwStateText(localState.gwStateText);
            appliedVersion = localState.version;
        }

        const uint32_t nowMs = millis();
        if (nowMs - lastTimeUiUpdateMs >= 1000) {
            char nowBuf[48];
            char deadlineBuf[96];
            formatUkNow(nowBuf, sizeof(nowBuf));
            formatDeadlineInfo(localState, deadlineBuf, sizeof(deadlineBuf));
            updateUkTimeText(nowBuf);
            updateDeadlineText(deadlineBuf);
            lastTimeUiUpdateMs = nowMs;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void fplTask(void *) {
    lastPollMs = 0;
    lastWifiRetryMs = 0;
    setSharedStatus("Connecting WiFi...", 0xFFCC66);

    for (;;) {
        const uint32_t now = millis();

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
                setSharedGwTiming(nextGw, true, deadlineUtc, hasDeadline);
                Serial.printf("GW state: live=%s next=%d\n", isLive ? "yes" : "no", nextGw);
            } else {
                setSharedGwStateText("GW live: ? | next: --");
                setSharedGwTiming(0, false, 0, false);
            }

            int gwPoints = 0;
            if (fetchAndPrintTeamSnapshot(gwPoints)) {
                setSharedGwPoints(gwPoints);
                setSharedStatus("FPL updated", 0x38D39F);
                Serial.printf("FPL GW points: %d\n", gwPoints);
            } else {
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

    lv_init();
    lv_tick_set_cb(lvglTickCb);

    lvglBuf = static_cast<uint8_t *>(heap_caps_malloc(kLvglBufPixels * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (!lvglBuf) {
        Serial.println("LVGL DMA buffer allocation failed");
        while (true) {
            delay(1000);
        }
    }

    lvglDisp = lv_display_create(kDisplayWidth, kDisplayHeight);
    lv_display_set_flush_cb(lvglDisp, lvglFlushCb);
    lv_display_set_buffers(lvglDisp, lvglBuf, nullptr, kLvglBufPixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *touchIndev = lv_indev_create();
    lv_indev_set_type(touchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchIndev, lvglTouchCb);

    createUi();
    sharedUiMutex = xSemaphoreCreateMutex();
    if (!sharedUiMutex) {
        Serial.println("Failed to create UI state mutex");
        while (true) {
            delay(1000);
        }
    }
    setSharedStatus("Booting...", 0xA0A0A0);
    setSharedGwStateText("GW live: ? | next: --");
    setSharedGwTiming(0, false, 0, false);

#if CONFIG_FREERTOS_UNICORE
    constexpr BaseType_t kUiCore = 0;
    constexpr BaseType_t kFplCore = 0;
#else
    constexpr BaseType_t kUiCore = 1;
    constexpr BaseType_t kFplCore = 0;
#endif

    xTaskCreatePinnedToCore(uiTask, "uiTask", 8192, nullptr, 2, &uiTaskHandle, kUiCore);
    xTaskCreatePinnedToCore(fplTask, "fplTask", 12288, nullptr, 1, &fplTaskHandle, kFplCore);

    if (!uiTaskHandle || !fplTaskHandle) {
        Serial.println("Failed to create worker tasks");
        while (true) {
            delay(1000);
        }
    }
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
