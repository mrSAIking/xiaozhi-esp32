#include "ili9488.h"

#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <initializer_list>

#include "avatar_math.h"
#include "config.h"

Ili9488::Ili9488() {
    constexpr size_t bytes = kWidth * kStripRows * 3;
    dma_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    complete_ = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(dma_ && complete_ ? ESP_OK : ESP_ERR_NO_MEM);
    spi_bus_config_t bus{};
    bus.sclk_io_num = pins::tft_sck;
    bus.mosi_io_num = pins::tft_mosi;
    bus.miso_io_num = bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.max_transfer_sz = bytes;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t cfg{};
    cfg.cs_gpio_num = pins::tft_cs;
    cfg.dc_gpio_num = pins::tft_dc;
    cfg.pclk_hz = CONFIG_ANIME_SPI_MHZ * 1000000;
    cfg.spi_mode = 0;
    cfg.lcd_cmd_bits = cfg.lcd_param_bits = 8;
    cfg.trans_queue_depth = 1;
    cfg.user_ctx = complete_;
    cfg.on_color_trans_done = [](esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                                 void* ctx) {
        BaseType_t woke = pdFALSE;
        xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &woke);
        return woke == pdTRUE;
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &cfg, &io_));

    ESP_ERROR_CHECK(gpio_set_direction(pins::tft_reset, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(pins::tft_reset, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(gpio_set_level(pins::tft_reset, 1));
    vTaskDelay(pdMS_TO_TICKS(120));
    const auto cmd = [this](int reg, std::initializer_list<uint8_t> data) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, reg, data.begin(), data.size()));
    };
    cmd(0xe0,
        {0x00, 0x03, 0x09, 0x08, 0x16, 0x0a, 0x3f, 0x78, 0x4c, 0x09, 0x0a, 0x08, 0x16, 0x1a, 0x0f});
    cmd(0xe1,
        {0x00, 0x16, 0x19, 0x03, 0x0f, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0e, 0x0d, 0x35, 0x37, 0x0f});
    cmd(0xc0, {0x17, 0x15});  // Power control and VCOM
    cmd(0xc1, {0x41});
    cmd(0xc5, {0x00, 0x12, 0x80});
    cmd(0x36, {0x48});  // Portrait, BGR glass
    cmd(0x3a, {0x66});  // 18-bit DBI, three high-aligned bytes per pixel
    cmd(0xb0, {0x00});
    cmd(0xb1, {0xa0});
    cmd(0xb4, {0x02});
    cmd(0xb6, {0x02, 0x02});
    cmd(0x11, {});
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29, {});
    vTaskDelay(pdMS_TO_TICKS(20));
}

void Ili9488::Flush(const lv_area_t& area, const uint8_t* pixels, unsigned stride) {
    const int width = area.x2 - area.x1 + 1;
    const uint8_t cols[] = {uint8_t(area.x1 >> 8), uint8_t(area.x1), uint8_t(area.x2 >> 8),
                            uint8_t(area.x2)};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, 0x2a, cols, sizeof(cols)));
    for (int y = area.y1; y <= area.y2;) {
        const int rows = std::min<int>(area.y2 - y + 1, kWidth * kStripRows / width);
        const int last = y + rows - 1;
        const uint8_t range[] = {uint8_t(y >> 8), uint8_t(y), uint8_t(last >> 8), uint8_t(last)};
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, 0x2b, range, sizeof(range)));
        auto* dst = dma_;
        for (int row = 0; row < rows; ++row, pixels += stride) {
            const auto* src = reinterpret_cast<const uint16_t*>(pixels);
            for (int x = 0; x < width; ++x, dst += 3)
                avatar::Expand565(src[x], dst);
        }
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_, 0x2c, dma_, dst - dma_));
        // This semaphore is given only by the final SPI transaction ISR.
        // Both the conversion buffer and LVGL flush remain owned until completion.
        xSemaphoreTake(complete_, portMAX_DELAY);
        y += rows;
    }
}
