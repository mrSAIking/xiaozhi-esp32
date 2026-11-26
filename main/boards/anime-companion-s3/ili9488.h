#pragma once

#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <cstddef>

// Single owner after initialization: the LVGL display task.
class Ili9488 {
public:
    static constexpr int kWidth = 320, kHeight = 480, kStripRows = 16;
    Ili9488();
    void RunColorTest();
    void Flush(const lv_area_t& area, const uint8_t* pixels, size_t stride, size_t capacity);

private:
    static constexpr size_t kDmaBytes = kWidth * kStripRows * 3;
    void WriteStrip(int x, int y, int width, int rows);
    esp_lcd_panel_io_handle_t io_ = nullptr;
    SemaphoreHandle_t complete_ = nullptr;
    uint8_t* dma_ = nullptr;
    bool first_flush_ = true;
};
