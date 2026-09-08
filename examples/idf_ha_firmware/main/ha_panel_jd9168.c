/*
 * ha_panel.h implementation for the D320C2403V1-MIPI panel: 3.2", 1024x768,
 * JD9168 driver, GT967 capacitive touch, SGM37604A backlight controller on
 * the module.
 *
 * Adapted from examples/idf_dsi_camera_preview/main/app_display.c, which
 * already proved this exact bring-up sequence on this exact board - see that
 * file for the full history of what was tried and why (RGB565 vs RGB888,
 * the DSI pixel clock, the self-test helpers). This is the same sequence
 * trimmed to just what the ha_panel.h interface needs; nothing about the
 * bring-up itself changed.
 *
 * The panel sits on an adapter board carrying a second TCA6408 at 0x20 (the
 * mainboard has one at 0x21). That expander gates everything:
 *
 *   P0  GPIO_EN  -> AP2281 load switch -> LCD_VDD
 *   P1  LCD_RES  -> JD9168 reset, active low
 *   P2  BL_EN    -> SGM37604A hardware enable
 *
 * So the order matters: power the panel, let rails settle, release reset,
 * then enable the backlight controller (it does not answer on I2C at all
 * before P2 is high). Panel reset is a register write via the expander, not
 * a GPIO, which is why the JD9168 is created with reset_gpio_num = -1.
 *
 * The frame buffer is RGB888 - RGB565 was tried and produces a blank screen,
 * the JD9168's init sequence evidently expects 24-bit pixels.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include "esp_lcd_jd9168.h"
#include "sgm37604a.h"
#include "tca6408.h"

#include "bsp/config.h"
#include "bsp/epdinky_p4_board.h"

#include "ha_panel.h"

static const char *TAG = "ha_panel_jd9168";

#define HA_PANEL_BPP 24

static tca6408_handle_t         s_exp;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_lcd_panel_handle_t   s_panel;

esp_err_t ha_panel_init(void)
{
    ESP_RETURN_ON_FALSE(!s_panel, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    /* The adapter shares the board I2C bus with everything else. */
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(bsp_i2c_get_handle(&bus), TAG, "I2C bus unavailable");

    ESP_RETURN_ON_ERROR(tca6408_init(bus, BSP_I2C_ADDR_LCD_EXPANDER, &s_exp), TAG,
                        "no expander at 0x%02X - is the display adapter fitted?",
                        BSP_I2C_ADDR_LCD_EXPANDER);

    /* Everything starts low and off: panel unpowered, held in reset,
     * backlight disabled. Then make P0..P2 outputs (0 = output on a
     * TCA6408). */
    ESP_RETURN_ON_ERROR(tca6408_set_output_val(s_exp, 0x00), TAG, "expander preset failed");
    ESP_RETURN_ON_ERROR(tca6408_set_config(s_exp, (uint8_t)~((1 << BSP_LCD_EXP_PIN_POWER_EN) |
                                                             (1 << BSP_LCD_EXP_PIN_PANEL_RST) |
                                                             (1 << BSP_LCD_EXP_PIN_BACKLIGHT_EN))),
                        TAG, "expander direction failed");

    ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_POWER_EN, 1), TAG,
                        "panel power on failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_PANEL_RST, 0), TAG,
                        "panel reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_PANEL_RST, 1), TAG,
                        "panel reset release failed");
    vTaskDelay(pdMS_TO_TICKS(120));   /* JD9168 needs time before it accepts commands */

    ESP_RETURN_ON_ERROR(tca6408_set_output_pin(s_exp, BSP_LCD_EXP_PIN_BACKLIGHT_EN, 1), TAG,
                        "backlight enable failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(sgm37604a_init(bus, SGM37604A_CURRENT_30MA), TAG,
                        "backlight init failed");

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id             = 0,
        .num_data_lanes     = BSP_DSI_DATA_LANES,
        .phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BSP_DSI_LANE_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus), TAG,
                        "could not create the DSI bus");

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io), TAG,
                        "could not create the DBI IO");

    esp_lcd_dpi_panel_config_t dpi_config =
        JD9168_1024_768_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_FMT_RGB888);

    jd9168_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus    = s_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_DSI_PIN_PANEL_RST,   /* not routed on this board */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = HA_PANEL_BPP,
        .vendor_config  = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9168(s_dbi_io, &panel_config, &s_panel), TAG,
                        "could not create the JD9168 panel");

    /* No esp_lcd_panel_reset() here - the panel was already hardware-reset
     * through the adapter's expander above. With reset_gpio_num set to NC
     * the driver would instead send a DCS software reset over DBI, which
     * puts the JD9168 back to its power-on defaults right before the init
     * sequence runs. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");

    ESP_LOGI(TAG, "Panel ready: %dx%d RGB888, %d DSI lanes at %d Mbps",
             BSP_DSI_LCD_H_RES, BSP_DSI_LCD_V_RES,
             BSP_DSI_DATA_LANES, BSP_DSI_LANE_BITRATE_MBPS);
    return ESP_OK;
}

esp_lcd_panel_handle_t ha_panel_get_handle(void)
{
    return s_panel;
}

uint16_t ha_panel_width(void)
{
    return BSP_DSI_LCD_H_RES;
}

uint16_t ha_panel_height(void)
{
    return BSP_DSI_LCD_V_RES;
}

esp_err_t ha_panel_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return sgm37604a_set_brightness_percent(percent);
}
