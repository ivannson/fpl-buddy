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
static lv_obj_t *circleObj = nullptr;
static lv_obj_t *labelStatus = nullptr;
static lv_obj_t *labelPoints = nullptr;

static uint32_t lastPollMs = 0;
static uint32_t lastWifiRetryMs = 0;
static TaskHandle_t uiTaskHandle = nullptr;
static TaskHandle_t fplTaskHandle = nullptr;

struct SharedUiState {
    int gwPoints = 0;
    bool hasGwPoints = false;
    uint32_t statusColor = 0xFFFFFF;
    char statusText[48] = "Booting...";
    uint32_t version = 0;
};

static SharedUiState sharedUiState;
static SemaphoreHandle_t sharedUiMutex = nullptr;

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

    int computedGwPoints = 0;
    for (size_t i = 0; i < pickCount; ++i) {
        computedGwPoints += picks[i].live.totalPoints * picks[i].multiplier;
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
        const int effectivePoints = p.live.totalPoints * p.multiplier;
        if (hasPlayerMeta) {
            Serial.printf("  [%2d] %-5s | %-15s | %-3s | curr:%2d | element:%4d | mult:%d%s%s | eff:%2d\n",
                          p.squadPosition, slot,
                          p.playerName.length() ? p.playerName.c_str() : "unknown",
                          p.positionName.length() ? p.positionName.c_str() : "?", p.live.totalPoints, p.elementId,
                          p.multiplier, p.isCaptain ? " C" : "", p.isViceCaptain ? " VC" : "", effectivePoints);
        } else {
            Serial.printf("  [%2d] %-5s | curr:%2d | element:%4d | mult:%d%s%s | eff:%2d\n", p.squadPosition, slot,
                          p.live.totalPoints, p.elementId, p.multiplier, p.isCaptain ? " C" : "",
                          p.isViceCaptain ? " VC" : "", effectivePoints);
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

    circleObj = lv_obj_create(screen);
    lv_obj_set_size(circleObj, center_circle, center_circle);
    lv_obj_center(circleObj);
    lv_obj_set_y(circleObj, -(center_circle / 2) - 26);
    lv_obj_set_style_radius(circleObj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circleObj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(circleObj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(circleObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(circleObj, LV_OBJ_FLAG_CLICKABLE);

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
    lv_obj_align(labelStatus, LV_ALIGN_BOTTOM_MID, 0, -18);

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

    for (;;) {
        lv_timer_handler();

        if (readSharedUiState(localState) && localState.version != appliedVersion) {
            if (localState.hasGwPoints) {
                updatePoints(localState.gwPoints);
            }
            updateStatus(localState.statusText, lv_color_hex(localState.statusColor));
            appliedVersion = localState.version;
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
