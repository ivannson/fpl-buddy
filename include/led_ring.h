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
void ledRingSetPulsePeriodMs(uint32_t periodMs);
void ledRingSetAnimationMode(uint8_t mode);
void ledRingTriggerNotificationForMs(uint32_t durationMs);
void ledRingTriggerNotification();
