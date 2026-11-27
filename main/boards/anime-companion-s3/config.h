#pragma once

#include <driver/gpio.h>

namespace pins {
constexpr auto tft_cs = GPIO_NUM_10;
constexpr auto tft_reset = GPIO_NUM_8;
constexpr auto tft_dc = GPIO_NUM_9;
constexpr auto tft_mosi = GPIO_NUM_11;
constexpr auto tft_sck = GPIO_NUM_12;
constexpr auto mic_sck = GPIO_NUM_5;
constexpr auto mic_ws = GPIO_NUM_4;
constexpr auto mic_sd = GPIO_NUM_6;
constexpr auto speaker_bclk = GPIO_NUM_15;
constexpr auto speaker_lrc = GPIO_NUM_16;
constexpr auto speaker_din = GPIO_NUM_17;
constexpr auto rgb = GPIO_NUM_48;
constexpr auto volume_up = GPIO_NUM_13;
constexpr auto volume_down = GPIO_NUM_14;
constexpr auto settings = GPIO_NUM_18;
constexpr auto boot = GPIO_NUM_0;
}  // namespace pins
