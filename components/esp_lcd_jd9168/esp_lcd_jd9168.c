#include "soc/soc_caps.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_jd9168.h"

#define JD9168_CMD_GS_BIT (1 << 0)
#define JD9168_CMD_SS_BIT (1 << 1)

typedef struct
{
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save surrent value of LCD_CMD_COLMOD register
    const jd9168_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct
    {
        unsigned int reset_level : 1;
    } flags;
    // To save the original functions of MIPI DPI panel
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
} jd9168_panel_t;

static const char *TAG = "jd9168";

static esp_err_t panel_jd9168_del(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9168_init(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9168_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9168_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_jd9168_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_jd9168_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_jd9168_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_jd9168_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_jd9168(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    jd9168_vendor_config_t *vendor_config = (jd9168_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->mipi_config.dpi_config && vendor_config->mipi_config.dsi_bus, ESP_ERR_INVALID_ARG, TAG,
                        "invalid vendor config");

    esp_err_t ret = ESP_OK;
    jd9168_panel_t *jd9168 = (jd9168_panel_t *)calloc(1, sizeof(jd9168_panel_t));
    ESP_RETURN_ON_FALSE(jd9168, ESP_ERR_NO_MEM, TAG, "no mem for jd9168 panel");

    if (panel_dev_config->reset_gpio_num >= 0)
    {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order)
    {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        jd9168->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        jd9168->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    jd9168->io = io;
    jd9168->init_cmds = vendor_config->init_cmds;
    jd9168->init_cmds_size = vendor_config->init_cmds_size;
    jd9168->reset_gpio_num = panel_dev_config->reset_gpio_num;
    jd9168->flags.reset_level = panel_dev_config->flags.reset_active_high;

    // Create MIPI DPI panel
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_dpi(vendor_config->mipi_config.dsi_bus, vendor_config->mipi_config.dpi_config, &panel_handle), err, TAG,
                      "create MIPI DPI panel failed");
    ESP_LOGD(TAG, "new MIPI DPI panel @%p", panel_handle);

    // Save the original functions of MIPI DPI panel
    jd9168->del = panel_handle->del;
    jd9168->init = panel_handle->init;
    // Overwrite the functions of MIPI DPI panel
    panel_handle->del = panel_jd9168_del;
    panel_handle->init = panel_jd9168_init;
    panel_handle->reset = panel_jd9168_reset;
    panel_handle->mirror = panel_jd9168_mirror;
    panel_handle->swap_xy = panel_jd9168_swap_xy;
    panel_handle->set_gap = panel_jd9168_set_gap;
    panel_handle->invert_color = panel_jd9168_invert_color;
    panel_handle->disp_on_off = panel_jd9168_disp_on_off;
    panel_handle->user_data = jd9168;
    *ret_panel = panel_handle;
    ESP_LOGD(TAG, "new jd9168 panel @%p", jd9168);

    return ESP_OK;

err:
    if (jd9168)
    {
        if (panel_dev_config->reset_gpio_num >= 0)
        {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(jd9168);
    }
    return ret;
}

static const jd9168_lcd_init_cmd_t vendor_specific_init_default[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xdf, (uint8_t[]){0x91, 0x68, 0xF9}, 3, 0},
    {0xde, (uint8_t[]){0x00}, 1, 0},
    {0xb2, (uint8_t[]){0x00, 0x7E}, 2, 0},
    {0xb3, (uint8_t[]){0x00, 0x7E}, 2, 0},
    {0xc1, (uint8_t[]){0x00, 0x10, 0x00, 0x00, 0x00, 0x00}, 6, 0},
    {0xbb, (uint8_t[]){0x02, 0x24, 0x07, 0x61, 0x19, 0x44, 0x44}, 7, 0},
    {0xbe, (uint8_t[]){0x1A, 0xF2}, 2, 0},
    {0xc3, (uint8_t[]){0x10, 0x17, 0x5A, 0x17, 0x5A, 0x05, 0x05, 0x05, 0x05, 0x15, 0x15, 0x31, 0x05, 0xDF}, 14, 0},
    {0xc4, (uint8_t[]){0x11, 0x80, 0x00, 0xDF, 0x09, 0x06, 0x14}, 7, 0},
    {0xcc, (uint8_t[]){0x35}, 1, 0},
    {0xce, (uint8_t[]){0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x0F, 0x03}, 23, 0},
    {0xcf, (uint8_t[]){0x00, 0x01, 0x40, 0x01, 0xCA, 0x01, 0xCA, 0x01, 0xCA}, 9, 0},

    {0xd0, (uint8_t[]){0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x03, 0x01, 0x05, 0x07, 0x09, 0x0B, 0x1E, 0x15, 0x1F, 0x1F, 0x15, 0x1F}, 23, 0},
    {0xd1, (uint8_t[]){0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x02, 0x00, 0x04, 0x06, 0x08, 0x0A, 0x1E, 0x15, 0x1F, 0x1F, 0x15, 0x1F}, 23, 0},
    {0xd2, (uint8_t[]){0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x02, 0x0A, 0x08, 0x06, 0x04, 0x1F, 0x15, 0x1F, 0x1F, 0x15, 0x1E}, 23, 0},
    {0xd3, (uint8_t[]){0x00, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x01, 0x03, 0x0B, 0x09, 0x07, 0x05, 0x1F, 0x15, 0x1F, 0x1F, 0x15, 0x1E}, 23, 0},

    {0xd4, (uint8_t[]){0x30, 0x00, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x11, 0x00, 0x01, 0xC0, 0x04, 0x01, 0x01, 0x11, 0x80, 0x01, 0xC0, 0x05, 0x01, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x06, 0x18, 0x02, 0xE3}, 37, 0},
    {0xd5, (uint8_t[]){0x68, 0x73, 0x00, 0x08, 0x08, 0x00, 0x03, 0x00}, 8, 0},
    {0xb7, (uint8_t[]){0x00, 0xD8, 0x00, 0x00, 0xD8, 0x00}, 6, 0},

    {0xc8, (uint8_t[]){
        0x7F, 0x69, 0x5A, 0x4E, 0x4A, 0x3B, 0x40, 0x2A, 0x44, 0x43,
        0x44, 0x63, 0x51, 0x59, 0x4C, 0x48, 0x3A, 0x28, 0x0F,
        0x7F, 0x69, 0x5A, 0x4E, 0x4A, 0x3B, 0x40, 0x2A, 0x44, 0x43,
        0x44, 0x63, 0x51, 0x59, 0x4C, 0x48, 0x3A, 0x28, 0x0F
    }, 38, 0},

    {0xde, (uint8_t[]){0x02}, 1, 0},
    {0xbb, (uint8_t[]){0x00, 0x5B, 0x5C, 0x41}, 4, 0},
    {0xb5, (uint8_t[]){0x00, 0x5A, 0x0A}, 3, 0},
    {0xc6, (uint8_t[]){0x22}, 1, 0},
    {0xd7, (uint8_t[]){0x12}, 1, 0},
    {0xe7, (uint8_t[]){0x00, 0x00}, 2, 0},

    {0xde, (uint8_t[]){0x04}, 1, 0},
    {0xcc, (uint8_t[]){0x02}, 1, 0},
    {0xe7, (uint8_t[]){0x01}, 1, 0},

    {0xde, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},

    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

static esp_err_t panel_jd9168_del(esp_lcd_panel_t *panel)
{
    jd9168_panel_t *jd9168 = (jd9168_panel_t *)panel->user_data;

    if (jd9168->reset_gpio_num >= 0)
    {
        gpio_reset_pin(jd9168->reset_gpio_num);
    }
    // Delete MIPI DPI panel
    jd9168->del(panel);

    ESP_LOGD(TAG, "del jd9168 panel @%p", jd9168);
    free(jd9168);

    return ESP_OK;
}

static esp_err_t panel_jd9168_init(esp_lcd_panel_t *panel)
{
    jd9168_panel_t *jd9168 = (jd9168_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = jd9168->io;
    const jd9168_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_cmd_overwritten = false;

    uint8_t ID[3];
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(io, 0x04, ID, 3), TAG, "read ID failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){
                                                                          jd9168->madctl_val,
                                                                      },
                                                  1),
                        TAG, "send command failed");

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (jd9168->init_cmds)
    {
        init_cmds = jd9168->init_cmds;
        init_cmds_size = jd9168->init_cmds_size;
    }
    else
    {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(jd9168_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++)
    {
        // Check if the command has been used or conflicts with the internal
        if (init_cmds[i].data_bytes > 0)
        {
            switch (init_cmds[i].cmd)
            {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                jd9168->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten)
            {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                         init_cmds[i].cmd);
            }
        }

        // Send command
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    ESP_RETURN_ON_ERROR(jd9168->init(panel), TAG, "init MIPI DPI panel failed");

    return ESP_OK;
}

static esp_err_t panel_jd9168_reset(esp_lcd_panel_t *panel)
{
    jd9168_panel_t *jd9168 = (jd9168_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = jd9168->io;

    // Perform hardware reset
    if (jd9168->reset_gpio_num >= 0)
    {
        gpio_set_level(jd9168->reset_gpio_num, !jd9168->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(jd9168->reset_gpio_num, jd9168->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(jd9168->reset_gpio_num, !jd9168->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    else if (io)
    { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_jd9168_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    jd9168_panel_t *jd9168 = (jd9168_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = jd9168->io;
    uint8_t command = 0;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    if (invert_color_data)
    {
        command = LCD_CMD_INVON;
    }
    else
    {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");

    return ESP_OK;
}

static esp_err_t panel_jd9168_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    jd9168_panel_t *jd9168 = (jd9168_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = jd9168->io;
    uint8_t madctl_val = jd9168->madctl_val;

    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_STATE, TAG, "invalid panel IO");

    // Control mirror through LCD command
    if (mirror_x)
    {
        madctl_val |= JD9168_CMD_GS_BIT;
    }
    else
    {
        madctl_val &= ~JD9168_CMD_GS_BIT;
    }
    if (mirror_y)
    {
        madctl_val |= JD9168_CMD_SS_BIT;
    }
    else
    {
        madctl_val &= ~JD9168_CMD_SS_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){madctl_val}, 1), TAG, "send command failed");
    jd9168->madctl_val = madctl_val;

    return ESP_OK;
}

static esp_err_t panel_jd9168_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    ESP_LOGE(TAG, "swap_xy is not supported by this panel");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_jd9168_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    ESP_LOGE(TAG, "set_gap is not supported by this panel");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_jd9168_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    jd9168_panel_t *jd9168 = (jd9168_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = jd9168->io;
    int command = 0;

    if (on_off)
    {
        command = LCD_CMD_DISPON;
    }
    else
    {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
