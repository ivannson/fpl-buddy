#include "power.h"

#include <driver/gpio.h>
#include <esp_system.h>

static int sysEnAtBoot = -1;

void powerInit() {
    const gpio_num_t sysEn = static_cast<gpio_num_t>(kPowerSysEnPin);
    // 1 after ESP.restart() if the hold from the previous boot kept the latch on through the reset.
    sysEnAtBoot = gpio_get_level(sysEn);
    // Pad hold lives in the RTC domain, so it survives resets and SYS_EN stays high through ESP.restart, watchdog
    // and OTA reboots (without it the pin floats during reset and a restart on battery turns the board off).
    // While a hold from the previous boot is active the pad is frozen, so configure it first, then release and
    // re-take the hold: the pad goes straight from its held level to high with no low glitch.
    // (Arduino 3.x ignores digitalWrite before pinMode, so pinMode must come first.)
    pinMode(kPowerSysEnPin, OUTPUT);
    digitalWrite(kPowerSysEnPin, HIGH);
    gpio_hold_dis(sysEn);
    gpio_hold_en(sysEn);
    pinMode(kPowerKeyPin, INPUT);
    analogSetPinAttenuation(kPowerBatAdcPin, ADC_11db);  // full range: the divided battery peaks at ~1.4 V
}

void powerLogState() {
    Serial.printf("[POWER] SYS_EN(GPIO%u)=%d (at boot %d) key(GPIO%u)=%s reset_reason=%d\n", kPowerSysEnPin,
                  gpio_get_level(static_cast<gpio_num_t>(kPowerSysEnPin)), sysEnAtBoot, kPowerKeyPin,
                  digitalRead(kPowerKeyPin) ? "released" : "pressed", static_cast<int>(esp_reset_reason()));
}

void powerOff() {
    Serial.println("[POWER] releasing battery latch");
    Serial.flush();
    gpio_hold_dis(static_cast<gpio_num_t>(kPowerSysEnPin));
    digitalWrite(kPowerSysEnPin, LOW);
}

float batteryVolts() {
    // Divider ratio 3 and calibration factor from Waveshare's BAT_Driver demo for this board.
    static constexpr float kDividerRatio = 3.0f;
    static constexpr float kCalibration = 0.990476f;
    static constexpr int kSamples = 16;
    uint32_t sumMv = 0;
    for (int i = 0; i < kSamples; ++i) {
        sumMv += analogReadMilliVolts(kPowerBatAdcPin);
    }
    return static_cast<float>(sumMv) / kSamples * kDividerRatio / 1000.0f / kCalibration;
}

int batteryPercent(float volts) {
    struct Point {
        float volts;
        int percent;
    };
    static constexpr Point kCurve[] = {{4.15f, 100}, {4.05f, 90}, {3.97f, 80}, {3.90f, 70}, {3.84f, 60},
                                       {3.80f, 50},  {3.77f, 40}, {3.74f, 30}, {3.71f, 20}, {3.67f, 10},
                                       {3.50f, 3},   {3.30f, 0}};
    static constexpr size_t kCount = sizeof(kCurve) / sizeof(kCurve[0]);
    if (volts >= kCurve[0].volts) {
        return 100;
    }
    for (size_t i = 1; i < kCount; ++i) {
        if (volts >= kCurve[i].volts) {
            const Point &hi = kCurve[i - 1];
            const Point &lo = kCurve[i];
            const float t = (volts - lo.volts) / (hi.volts - lo.volts);
            return lo.percent + static_cast<int>(lroundf(t * (hi.percent - lo.percent)));
        }
    }
    return 0;
}
