#pragma once

#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

// Single owner after initialization: the LVGL display task.
class Ili9488 {
public:
    static constexpr int kWidth = 320, kHeight = 480, kStripRows = 8;
    Ili9488();
    void Flush(const lv_area_t& area, const uint8_t* pixels, unsigned stride);

private:
    esp_lcd_panel_io_handle_t io_ = nullptr;
    SemaphoreHandle_t complete_ = nullptr;
    uint8_t* dma_ = nullptr;
};
