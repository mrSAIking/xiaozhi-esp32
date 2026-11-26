#include "ili9488.h"

#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <algorithm>
#include <initializer_list>

#include "avatar_math.h"
#include "config.h"

namespace {
constexpr char kTag[] = "AnimeLCD";
constexpr uint8_t kMadctl = 0x48;  // MX + BGR, MV clear for portrait addressing.
constexpr uint8_t kColmod = 0x66;  // 18-bit DBI, one byte per color component.

void DelayMs(unsigned ms) {
    // Add a tick to cover the partial tick at the start of the delay.
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}
}  // namespace

Ili9488::Ili9488() {
    dma_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(64, kDmaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    complete_ = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(dma_ && complete_ ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(kTag, "SPI3 mode 0, %d MHz, CS=%d DC=%d RESET=%d MOSI=%d SCK=%d, DMA=%u bytes",
             CONFIG_ANIME_SPI_MHZ, pins::tft_cs, pins::tft_dc, pins::tft_reset, pins::tft_mosi,
             pins::tft_sck, static_cast<unsigned>(kDmaBytes));
    spi_bus_config_t bus{};
    bus.sclk_io_num = pins::tft_sck;
    bus.mosi_io_num = pins::tft_mosi;
    bus.miso_io_num = bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kDmaBytes;
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
    DelayMs(20);
    ESP_ERROR_CHECK(gpio_set_level(pins::tft_reset, 1));
    DelayMs(120);
    ESP_LOGI(kTag, "Hardware reset released and settled");
    const auto cmd = [this](int reg, std::initializer_list<uint8_t> data) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, reg, data.begin(), data.size()));
    };
    cmd(0x28, {});  // Keep the panel blank while registers are configured.
    cmd(0xe0,
        {0x00, 0x03, 0x09, 0x08, 0x16, 0x0a, 0x3f, 0x78, 0x4c, 0x09, 0x0a, 0x08, 0x16, 0x1a, 0x0f});
    cmd(0xe1,
        {0x00, 0x16, 0x19, 0x03, 0x0f, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0e, 0x0d, 0x35, 0x37, 0x0f});
    cmd(0xc0, {0x17, 0x15});  // Power control and VCOM
    cmd(0xc1, {0x41});
    cmd(0xc5, {0x00, 0x12, 0x80});
    cmd(0x36, {kMadctl});
    cmd(0x3a, {kColmod});
    cmd(0xb0, {0x00});
    cmd(0xb1, {0xa0});
    cmd(0xb4, {0x02});
    cmd(0xb6, {0x02, 0x02, 0x3b});  // 0x3b encodes 480 gate lines.
    ESP_LOGI(kTag, "Configured %dx%d portrait, MADCTL=0x%02x COLMOD=0x%02x", kWidth, kHeight,
             kMadctl, kColmod);
    cmd(0x11, {});
    DelayMs(120);
    cmd(0x13, {});  // Normal display mode.
    cmd(0x29, {});
    DelayMs(20);
    ESP_LOGI(kTag, "Sleep out and display on commands sent");
}

void Ili9488::WriteStrip(int x, int y, int width, int rows) {
    const int last_x = x + width - 1;
    const int last_y = y + rows - 1;
    const uint8_t cols[] = {uint8_t(x >> 8), uint8_t(x), uint8_t(last_x >> 8), uint8_t(last_x)};
    const uint8_t lines[] = {uint8_t(y >> 8), uint8_t(y), uint8_t(last_y >> 8), uint8_t(last_y)};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, 0x2a, cols, sizeof(cols)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, 0x2b, lines, sizeof(lines)));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_, 0x2c, dma_, width * rows * 3));
    // The SPI ISR signals after the final color transaction. Never reuse this
    // buffer after a timeout because the driver may still own it.
    if (xSemaphoreTake(complete_, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(kTag, "DMA completion timed out at (%d,%d), %dx%d", x, y, width, rows);
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }
}

void Ili9488::RunColorTest() {
    constexpr struct {
        const char* name;
        uint16_t rgb565;
    } colors[] = {
        {"red", 0xf800}, {"green", 0x07e0}, {"blue", 0x001f}, {"white", 0xffff}, {"black", 0x0000}};
    ESP_LOGI(kTag, "Starting pre-LVGL color test");
    for (const auto& color : colors) {
        ESP_LOGI(kTag, "Color test: %s", color.name);
        for (size_t offset = 0; offset < kDmaBytes; offset += 3)
            avatar::Expand565(color.rgb565, dma_ + offset);
        for (int y = 0; y < kHeight; y += kStripRows)
            WriteStrip(0, y, kWidth, std::min(kStripRows, kHeight - y));
        DelayMs(700);
    }
    ESP_LOGI(kTag, "Color test transfers complete");
}

void Ili9488::Flush(const lv_area_t& area, const uint8_t* pixels, size_t stride, size_t capacity) {
    const int width = lv_area_get_width(&area);
    const int height = lv_area_get_height(&area);
    if (area.x1 < 0 || area.y1 < 0 || area.x2 >= kWidth || area.y2 >= kHeight || !pixels ||
        reinterpret_cast<uintptr_t>(pixels) % alignof(uint16_t) != 0 ||
        !avatar::FitsRgb565Buffer(width, height, stride, capacity)) {
        ESP_LOGE(kTag, "Invalid LVGL flush (%d,%d)-(%d,%d) stride=%zu capacity=%zu", int(area.x1),
                 int(area.y1), int(area.x2), int(area.y2), stride, capacity);
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }
    if (first_flush_) {
        first_flush_ = false;
        ESP_LOGI(kTag, "First LVGL flush %dx%d at (%d,%d) stride=%zu capacity=%zu", width, height,
                 int(area.x1), int(area.y1), stride, capacity);
    }
    for (int y = area.y1; y <= area.y2;) {
        const int rows = std::min<int>(area.y2 - y + 1, kWidth * kStripRows / width);
        auto* dst = dma_;
        for (int row = 0; row < rows; ++row, pixels += stride) {
            const auto* src = reinterpret_cast<const uint16_t*>(pixels);
            for (int x = 0; x < width; ++x, dst += 3)
                avatar::Expand565(src[x], dst);
        }
        WriteStrip(area.x1, y, width, rows);
        y += rows;
    }
}
