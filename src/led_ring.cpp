#include "led_ring.h"

#include "fpl_config.h"

#if FPL_LED_RING_ENABLED

#include <Adafruit_NeoPixel.h>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

namespace {

struct LedRingState {
    bool initialized = false;
    int rankTrend = 0;  // +1 = rank up, -1 = rank down, 0 = no signal
    bool deadlineCountdownEnabled = false;
    time_t deadlineUtc = 0;
    uint8_t animationMode = kLedRingAnimBreathing;
    uint8_t head = 0;
    uint32_t lastRotateMs = 0;
    uint32_t pulsePeriodMs = FPL_LED_RING_PULSE_PERIOD_MS;
    bool flashActive = false;
    bool flashOn = false;
    uint32_t flashEndMs = 0;
    uint32_t nextFlashStepMs = 0;
    SemaphoreHandle_t mutex = nullptr;
};

static LedRingState gState;
static Adafruit_NeoPixel gRing(FPL_LED_RING_LED_COUNT, FPL_LED_RING_PIN, NEO_GRB + NEO_KHZ800);

static constexpr float kTwoPi = 6.283185307f;
static constexpr uint8_t kPurpleR = 180;
static constexpr uint8_t kPurpleG = 0;
static constexpr uint8_t kPurpleB = 180;
static constexpr int32_t kDeadlineCountdownWindowSec = 3600;
static constexpr int32_t kDeadlineStepSec = 225;  // 3.75 minutes

static uint8_t clampByte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

static uint8_t scaleChannel(uint8_t channel, float scale) {
    const int scaled = static_cast<int>(static_cast<float>(channel) * scale);
    return clampByte(scaled);
}

static uint32_t normalizePulsePeriodMs(uint32_t periodMs) {
    if (periodMs < 50U) {
        return 50U;
    }
    return periodMs;
}

static uint32_t safeSpinIntervalMs() {
    return (FPL_LED_RING_SPIN_INTERVAL_MS > 0U) ? FPL_LED_RING_SPIN_INTERVAL_MS : 1U;
}

static float pulseScale(uint32_t nowMs, uint32_t periodMs) {
    const float phase = static_cast<float>(nowMs % periodMs) / static_cast<float>(periodMs);
    return 0.08f + (0.92f * (0.5f + (0.5f * sinf(kTwoPi * phase))));
}

static uint32_t safeFlashDurationMs() {
    return (FPL_LED_RING_NOTIFICATION_FLASH_MS > 0U) ? FPL_LED_RING_NOTIFICATION_FLASH_MS : 1U;
}

static uint8_t safeFlashCycles() {
    const uint32_t flashes = (FPL_LED_RING_NOTIFICATION_FLASH_COUNT > 0U) ? FPL_LED_RING_NOTIFICATION_FLASH_COUNT : 1U;
    return (flashes > 255U) ? 255U : static_cast<uint8_t>(flashes);
}

static bool timeReached(uint32_t nowMs, uint32_t targetMs) {
    return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

static void clearRing() {
    gRing.clear();
    gRing.show();
}

static void renderSolid(uint8_t r, uint8_t g, uint8_t b) {
    for (uint16_t i = 0; i < gRing.numPixels(); ++i) {
        gRing.setPixelColor(i, r, g, b);
    }
    gRing.show();
}

static float cometTailScaleFromDistance(uint8_t distance) {
    switch (distance) {
        case 0: return 1.00f;
        case 1: return 0.45f;
        case 2: return 0.18f;
        case 3: return 0.08f;
        default: return 0.0f;
    }
}

static void renderRankTrendBreathing(uint32_t nowMs, int rankTrend, uint32_t pulsePeriodMs) {
    if (rankTrend == 0) {
        clearRing();
        return;
    }

    const uint8_t baseR = rankTrend > 0 ? 0 : 255;
    const uint8_t baseG = rankTrend > 0 ? 255 : 0;
    const uint8_t ledCount = static_cast<uint8_t>(gRing.numPixels());
    if (ledCount == 0) {
        return;
    }

    const float pulse = pulseScale(nowMs, pulsePeriodMs);
    for (uint8_t i = 0; i < ledCount; ++i) {
        gRing.setPixelColor(i, scaleChannel(baseR, pulse), scaleChannel(baseG, pulse), 0);
    }
    gRing.show();
}

static void renderRankTrendDimNotch(uint32_t nowMs, int rankTrend, uint8_t head, uint32_t pulsePeriodMs) {
    if (rankTrend == 0) {
        clearRing();
        return;
    }

    const uint8_t baseR = rankTrend > 0 ? 0 : 255;
    const uint8_t baseG = rankTrend > 0 ? 255 : 0;
    const uint8_t ledCount = static_cast<uint8_t>(gRing.numPixels());
    if (ledCount == 0) {
        return;
    }

    const float pulse = pulseScale(nowMs, pulsePeriodMs);

    const float dimmerScale = 0.22f;
    for (uint8_t i = 0; i < ledCount; ++i) {
        const bool isDimmer = (i == head);
        const float scale = pulse * (isDimmer ? dimmerScale : 1.0f);
        gRing.setPixelColor(i, scaleChannel(baseR, scale), scaleChannel(baseG, scale), 0);
    }

    gRing.show();
}

static void renderRankTrendComet(uint32_t nowMs, int rankTrend, uint8_t head, uint32_t pulsePeriodMs) {
    if (rankTrend == 0) {
        clearRing();
        return;
    }

    const uint8_t baseR = rankTrend > 0 ? 0 : 255;
    const uint8_t baseG = rankTrend > 0 ? 255 : 0;
    const uint8_t ledCount = static_cast<uint8_t>(gRing.numPixels());
    if (ledCount == 0) {
        return;
    }

    const float pulse = pulseScale(nowMs, pulsePeriodMs);
    for (uint8_t i = 0; i < ledCount; ++i) {
        const uint8_t distance = (i >= head) ? static_cast<uint8_t>(i - head)
                                             : static_cast<uint8_t>(ledCount + i - head);
        const float scale = pulse * cometTailScaleFromDistance(distance);
        gRing.setPixelColor(i, scaleChannel(baseR, scale), scaleChannel(baseG, scale), 0);
    }
    gRing.show();
}

static void renderDeadlineCountdown(uint32_t nowMs, int32_t secRemaining) {
    const uint16_t ledCount = gRing.numPixels();
    if (ledCount == 0) {
        return;
    }

    if (secRemaining <= 0) {
        clearRing();
        return;
    }
    const uint8_t baseR = (secRemaining <= 900) ? 255 : kPurpleR;
    const uint8_t baseG = (secRemaining <= 900) ? 0 : kPurpleG;
    const uint8_t baseB = (secRemaining <= 900) ? 0 : kPurpleB;

    int litCount = (secRemaining + (kDeadlineStepSec - 1)) / kDeadlineStepSec;
    if (litCount < 0) {
        litCount = 0;
    } else if (litCount > static_cast<int>(ledCount)) {
        litCount = static_cast<int>(ledCount);
    }
    const bool blinkOn = ((nowMs / 500U) % 2U) == 0U;

    gRing.clear();
    const int rotationOffset = static_cast<int>(ledCount) / 4;  // move final segment from 9 o'clock to 12 o'clock
    for (int i = 0; i < litCount; ++i) {
        const bool isCurrent = (i == litCount - 1);
        const uint16_t physicalIndex = static_cast<uint16_t>(
            (rotationOffset - i + static_cast<int>(ledCount)) % static_cast<int>(ledCount));
        if (isCurrent && !blinkOn) {
            gRing.setPixelColor(physicalIndex, 0, 0, 0);
        } else {
            gRing.setPixelColor(physicalIndex, baseR, baseG, baseB);
        }
    }

    gRing.show();
}

}  // namespace

void ledRingInit() {
    if (gState.initialized) {
        return;
    }

    gState.mutex = xSemaphoreCreateMutex();
    if (!gState.mutex) {
        return;
    }

    gRing.begin();
    gRing.setBrightness(clampByte(FPL_LED_RING_MAX_BRIGHTNESS));
    clearRing();

    if (xSemaphoreTake(gState.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        gState.initialized = true;
        gState.animationMode = FPL_LED_RING_DEFAULT_ANIMATION;
        gState.head = 0;
        gState.lastRotateMs = millis();
        gState.pulsePeriodMs = normalizePulsePeriodMs(FPL_LED_RING_PULSE_PERIOD_MS);
        gState.flashActive = false;
        gState.flashOn = false;
        gState.flashEndMs = 0;
        gState.nextFlashStepMs = 0;
        xSemaphoreGive(gState.mutex);
    }
}

void ledRingSetRankTrend(int rankDiff, bool hasRankData) {
    if (!gState.initialized || !gState.mutex) {
        return;
    }

    const int trend = (!hasRankData || rankDiff == 0) ? 0 : (rankDiff > 0 ? 1 : -1);
    if (xSemaphoreTake(gState.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    gState.rankTrend = trend;
    xSemaphoreGive(gState.mutex);
}

void ledRingSetDeadlineCountdown(bool enabled, time_t deadlineUtc) {
    if (!gState.initialized || !gState.mutex) {
        return;
    }
    if (xSemaphoreTake(gState.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    gState.deadlineCountdownEnabled = enabled;
    gState.deadlineUtc = deadlineUtc;
    xSemaphoreGive(gState.mutex);
}

void ledRingSetPulsePeriodMs(uint32_t periodMs) {
    if (!gState.initialized || !gState.mutex) {
        return;
    }
    if (xSemaphoreTake(gState.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    gState.pulsePeriodMs = normalizePulsePeriodMs(periodMs);
    xSemaphoreGive(gState.mutex);
}

void ledRingSetAnimationMode(uint8_t mode) {
    if (!gState.initialized || !gState.mutex) {
        return;
    }
    if (xSemaphoreTake(gState.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    if (mode > kLedRingAnimComet) {
        mode = kLedRingAnimBreathing;
    }
    gState.animationMode = mode;
    xSemaphoreGive(gState.mutex);
}

void ledRingTriggerNotificationForMs(uint32_t durationMs) {
    if (!gState.initialized || !gState.mutex) {
        return;
    }
    if (xSemaphoreTake(gState.mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }

    const uint32_t nowMs = millis();
    const uint32_t blinkIntervalMs = safeFlashDurationMs();
    if (durationMs == 0U) {
        durationMs = blinkIntervalMs;
    }

    const uint32_t requestedEndMs = nowMs + durationMs;
    if (!gState.flashActive || timeReached(nowMs, gState.flashEndMs)) {
        gState.flashActive = true;
        gState.flashOn = true;
        gState.flashEndMs = requestedEndMs;
        gState.nextFlashStepMs = nowMs + blinkIntervalMs;
        xSemaphoreGive(gState.mutex);
        return;
    }

    if (static_cast<int32_t>(requestedEndMs - gState.flashEndMs) > 0) {
        gState.flashEndMs = requestedEndMs;
    }
    xSemaphoreGive(gState.mutex);
}

void ledRingTriggerNotification() {
    const uint32_t totalMs = static_cast<uint32_t>(safeFlashCycles()) * 2U * safeFlashDurationMs();
    ledRingTriggerNotificationForMs(totalMs);
}

void ledRingTick(uint32_t nowMs) {
    if (!gState.initialized || !gState.mutex) {
        return;
    }
    if (xSemaphoreTake(gState.mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }

    const uint16_t pixelCount = gRing.numPixels();
    if (pixelCount == 0U) {
        xSemaphoreGive(gState.mutex);
        return;
    }

    const uint32_t spinIntervalMs = safeSpinIntervalMs();
    if ((nowMs - gState.lastRotateMs) >= spinIntervalMs) {
        const uint32_t steps = (nowMs - gState.lastRotateMs) / spinIntervalMs;
        gState.head = static_cast<uint8_t>((gState.head + steps) % pixelCount);
        gState.lastRotateMs += steps * spinIntervalMs;
    }

    if (gState.flashActive && timeReached(nowMs, gState.flashEndMs)) {
        gState.flashActive = false;
        gState.flashOn = false;
    } else if (gState.flashActive && timeReached(nowMs, gState.nextFlashStepMs)) {
        gState.flashOn = !gState.flashOn;
        gState.nextFlashStepMs = nowMs + safeFlashDurationMs();
        if (timeReached(gState.nextFlashStepMs, gState.flashEndMs)) {
            gState.flashActive = false;
            gState.flashOn = false;
        }
    }

    const bool flashOn = gState.flashActive && gState.flashOn;
    const bool flashWindowActive = gState.flashActive;
    const bool deadlineCountdownEnabled = gState.deadlineCountdownEnabled;
    const time_t deadlineUtc = gState.deadlineUtc;
    const int rankTrend = gState.rankTrend;
    const uint8_t animationMode = gState.animationMode;
    const uint8_t head = gState.head;
    const uint32_t pulsePeriodMs = normalizePulsePeriodMs(gState.pulsePeriodMs);
    xSemaphoreGive(gState.mutex);

    if (flashWindowActive) {
        if (flashOn) {
            renderSolid(kPurpleR, kPurpleG, kPurpleB);
        } else {
            clearRing();
        }
        return;
    }

    if (deadlineCountdownEnabled) {
        const time_t nowUtc = time(nullptr);
        if (nowUtc > 100000 && deadlineUtc > 0) {
            const int64_t secRemaining64 = static_cast<int64_t>(deadlineUtc) - static_cast<int64_t>(nowUtc);
            if (secRemaining64 <= kDeadlineCountdownWindowSec) {
                renderDeadlineCountdown(nowMs, static_cast<int32_t>(secRemaining64));
                return;
            }
        }
    }

    if (animationMode == kLedRingAnimComet) {
        renderRankTrendComet(nowMs, rankTrend, head, pulsePeriodMs);
        return;
    }
    if (animationMode == kLedRingAnimDimNotch) {
        renderRankTrendDimNotch(nowMs, rankTrend, head, pulsePeriodMs);
        return;
    }
    renderRankTrendBreathing(nowMs, rankTrend, pulsePeriodMs);
}

#else

void ledRingInit() {}
void ledRingTick(uint32_t) {}
void ledRingSetRankTrend(int, bool) {}
void ledRingSetDeadlineCountdown(bool, time_t) {}
void ledRingSetPulsePeriodMs(uint32_t) {}
void ledRingSetAnimationMode(uint8_t) {}
void ledRingTriggerNotificationForMs(uint32_t) {}
void ledRingTriggerNotification() {}

#endif
