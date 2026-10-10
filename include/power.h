#pragma once

#include <Arduino.h>

// Battery power latch on the Waveshare ESP32-S3-Touch-LCD-1.46(B). The PWR button only powers the board while it is
// held; firmware must drive SYS_EN high to keep the battery rail on after release (see Waveshare's PWR_Key demo).
static constexpr uint8_t kPowerKeyPin = 6;      // PWR button, low while pressed
static constexpr uint8_t kPowerSysEnPin = 7;    // SYS_EN battery latch, high keeps the board on
// Battery voltage through a 200k/100k divider into ADC1_CH7 (Waveshare's BAT_Driver demo). ADC1, so it keeps
// working while WiFi is on.
static constexpr uint8_t kPowerBatAdcPin = 8;

// Latches battery power on. Call first thing in setup(): booting from USB (or a PWR press) then keeps running on
// battery after USB is unplugged, and the latch is held through ESP.restart().
void powerInit();

// Prints the latch and PWR key levels (the latch should read 1).
void powerLogState();

// Releases the battery latch. On battery the board switches off; on USB it keeps running until USB is removed.
void powerOff();

// Battery voltage in volts (averaged over a burst of samples). On USB this is the charger's output, so it reads
// near full whether or not a battery is fitted.
float batteryVolts();

// Rough state of charge, 0-100, from a resting single-cell LiPo discharge curve.
int batteryPercent(float volts);
