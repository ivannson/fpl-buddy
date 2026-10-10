#pragma once

#include <Arduino.h>
#include <time.h>

static constexpr uint8_t kLedRingAnimBreathing = 0;
static constexpr uint8_t kLedRingAnimDimNotch = 1;
static constexpr uint8_t kLedRingAnimComet = 2;

void ledRingInit();
void ledRingTick(uint32_t nowMs);
void ledRingSetRankTrend(int rankDiff, bool hasRankData);
void ledRingSetDeadlineCountdown(bool enabled, time_t deadlineUtc);
// Whether the gameweek is live and whether a match is in play right now (for the rank LED filters).
void ledRingSetLiveState(bool gameweekLive, bool matchInPlay);
// User settings: brightness in percent of FPL_LED_RING_MAX_BRIGHTNESS, and when the rank trend may show.
void ledRingApplySettings(uint8_t rankBrightnessPct, uint8_t deadlineBrightnessPct,
                          uint8_t notificationBrightnessPct, bool rankOnlyLiveGw, bool rankOnlyMatchLive);
void ledRingSetPulsePeriodMs(uint32_t periodMs);
void ledRingSetAnimationMode(uint8_t mode);
void ledRingTriggerNotificationForMs(uint32_t durationMs);
void ledRingTriggerNotification();
