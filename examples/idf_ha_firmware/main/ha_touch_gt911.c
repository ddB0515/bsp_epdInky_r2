/*
 * ha_touch.h implementation for the GT967: a Goodix GT9xx part that speaks
 * the same register protocol as the GT911, so the stock esp_lcd_touch_gt911
 * driver handles it. Adapted from
 * examples/idf_dsi_camera_preview/main/app_ui.c's touch_init()/touch_read_cb().
 */

#include "esp_check.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "ha_touch.h"

static const char *TAG = "ha_touch_gt911";

static esp_lcd_touch_handle_t s_touch;

esp_err_t ha_touch_init(void)
{
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&bus), TAG, "I2C bus unavailable");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = BSP_I2C_ADDR_TOUCH;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch IO failed");

    const esp_lcd_touch_config_t cfg = {
        .x_max        = BSP_DSI_LCD_H_RES,
        .y_max        = BSP_DSI_LCD_V_RES,
        .rst_gpio_num = BSP_DSI_PIN_TOUCH_RST,
        .int_gpio_num = BSP_DSI_PIN_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };

    esp_err_t err = esp_lcd_touch_new_i2c_gt911(io, &cfg, &s_touch);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(io);
        return err;
    }

    ESP_LOGI(TAG, "GT967 touch ready at 0x%02X", BSP_I2C_ADDR_TOUCH);
    return ESP_OK;
}

void ha_touch_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x = 0, y = 0;
    uint8_t  count = 0;

    esp_lcd_touch_read_data(s_touch);
    bool pressed = esp_lcd_touch_get_coordinates(s_touch, &x, &y, NULL, &count, 1);

    if (pressed && count > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
