#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"

// ============================================================================
// Xiaomi Mi LED Desk Lamp 1S (MJTD01SYL / Mango1S) Hardware GPIO Configuration
// ============================================================================

// Power rail enable: Active HIGH (enables buck drivers for the LEDs)
#define LAMP_POWER_PIN          GPIO_NUM_12

// PWM Channels:
// - Cold White LED: GPIO2
// - Warm White LED: GPIO4
#define LAMP_COLD_PIN           GPIO_NUM_2
#define LAMP_WARM_PIN           GPIO_NUM_4

// LEDC PWM Configuration
#define LAMP_PWM_FREQ_HZ        40000               // 40 kHz flicker-free, no audible coil whine
#define LAMP_PWM_TIMER          LEDC_TIMER_0
#define LAMP_PWM_MODE           LEDC_LOW_SPEED_MODE
#define LAMP_PWM_RES            LEDC_TIMER_10_BIT   // Resolution: 0..1023
#define LAMP_PWM_MAX_DUTY       1023

#define LAMP_COLD_LEDC_CH       LEDC_CHANNEL_0
#define LAMP_WARM_LEDC_CH       LEDC_CHANNEL_1

// Rotary Encoder & Buttons:
// - Knob Push Button: GPIO33 (Active LOW, internal pull-up)
// - Bottom Pinhole Reset Button: GPIO16 (Active LOW, internal pull-up, from stock 0x40115a24)
// - Rotary Encoder: GPIO26 & GPIO27 (Standard Mango1S)
#define LAMP_BTN_PIN            GPIO_NUM_33
#define LAMP_RESET_BTN_PIN      GPIO_NUM_16
#define LAMP_ROTARY_A_PIN       GPIO_NUM_26
#define LAMP_ROTARY_B_PIN       GPIO_NUM_27

// Matter Lighting Limits (Mireds = 1,000,000 / Kelvin)
// 153 mireds = ~6536 K (Coolest)
// 370 mireds = ~2702 K (Warmest)
#define LAMP_MIN_MIREDS         153
#define LAMP_MAX_MIREDS         370
#define LAMP_DEFAULT_MIREDS     250 // ~4000 K

// Level Control Limits
#define LAMP_MIN_LEVEL          1
#define LAMP_MAX_LEVEL          254
#define LAMP_DEFAULT_LEVEL      200
