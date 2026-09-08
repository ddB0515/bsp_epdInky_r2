#include "tps65185.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "TPS65185";

/*******************************************************************************
 * Private Structure
 ******************************************************************************/

struct tps65185_dev {
    i2c_master_dev_handle_t i2c_dev;
    tps65185_gpio_config_t gpio_pins;
};

/*******************************************************************************
 * Low-Level Register Access
 ******************************************************************************/

esp_err_t tps65185_read_register(tps65185_handle_t handle, uint8_t reg, uint8_t *value)
{
    if (handle == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_master_transmit_receive(handle->i2c_dev, &reg, 1, value, 1, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read register 0x%02X: %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t tps65185_write_register(tps65185_handle_t handle, uint8_t reg, uint8_t value)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t write_buf[2] = {reg, value};
    esp_err_t ret = i2c_master_transmit(handle->i2c_dev, write_buf, sizeof(write_buf), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write register 0x%02X: %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t tps65185_modify_register(tps65185_handle_t handle, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t reg_value;
    esp_err_t ret = tps65185_read_register(handle, reg, &reg_value);
    if (ret != ESP_OK) {
        return ret;
    }

    reg_value = (reg_value & ~mask) | (value & mask);
    return tps65185_write_register(handle, reg, reg_value);
}

/*******************************************************************************
 * Initialization Functions
 ******************************************************************************/

esp_err_t tps65185_init(i2c_master_bus_handle_t bus_handle, 
                        const tps65185_gpio_config_t *gpio_cfg,
                        tps65185_handle_t *handle)
{
    if (bus_handle == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tps65185_dev *dev = calloc(1, sizeof(struct tps65185_dev));
    if (dev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for device");
        return ESP_ERR_NO_MEM;
    }

    // Store GPIO configuration
    if (gpio_cfg != NULL) {
        memcpy(&dev->gpio_pins, gpio_cfg, sizeof(tps65185_gpio_config_t));
    } else {
        // Set all pins to -1 (not used)
        dev->gpio_pins.wakeup_pin = GPIO_NUM_NC;
        dev->gpio_pins.pwrup_pin = GPIO_NUM_NC;
        dev->gpio_pins.vcom_ctrl_pin = GPIO_NUM_NC;
        dev->gpio_pins.int_pin = GPIO_NUM_NC;
        dev->gpio_pins.pwr_good_pin = GPIO_NUM_NC;
    }

    // Configure WAKEUP GPIO if specified
    if (dev->gpio_pins.wakeup_pin != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << dev->gpio_pins.wakeup_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(dev->gpio_pins.wakeup_pin, 0);  // Start with device in sleep
    }

    // Configure PWRUP GPIO if specified
    if (dev->gpio_pins.pwrup_pin != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << dev->gpio_pins.pwrup_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(dev->gpio_pins.pwrup_pin, 0);  // Start with rails off
    }

    // Configure VCOM_CTRL GPIO if specified
    if (dev->gpio_pins.vcom_ctrl_pin != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << dev->gpio_pins.vcom_ctrl_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(dev->gpio_pins.vcom_ctrl_pin, 0);
    }

    // Configure INT GPIO as input if specified
    if (dev->gpio_pins.int_pin != GPIO_NUM_NC) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << dev->gpio_pins.int_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    // Configure PWRGOOD GPIO as input if specified
    if (dev->gpio_pins.pwr_good_pin != GPIO_NUM_NC) {
        /*
         * PWR_GOOD is an open-drain output on the TPS65185: it pulls low when
         * the rails are bad and simply releases when they are good.  It needs
         * a pull-up to read as anything but low, and boards do not always
         * fit one - the epdInky routes it straight to the GPIO with no
         * resistor - so enable the internal pull-up rather than depending on
         * the board.  Without it the pin floats low forever and the rails
         * look permanently bad however healthy they are.
         */
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << dev->gpio_pins.pwr_good_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    // Add I2C device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TPS65185_I2C_ADDR,
        .scl_speed_hz = TPS65185_I2C_CLK_SPEED,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        free(dev);
        return ret;
    }

    // Wake up the device first to allow I2C communication
    if (dev->gpio_pins.wakeup_pin != GPIO_NUM_NC) {
        gpio_set_level(dev->gpio_pins.wakeup_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(10));  // Wait for device to wake up
    }

    // Verify device is present by reading revision ID
    uint8_t rev_id;
    ret = tps65185_read_register(dev, TPS65185_REG_REVID, &rev_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read revision ID, device not responding");
        i2c_master_bus_rm_device(dev->i2c_dev);
        free(dev);
        return ret;
    }

    ESP_LOGI(TAG, "TPS65185 initialized, Revision ID: 0x%02X", rev_id);
    *handle = dev;
    return ESP_OK;
}

esp_err_t tps65185_deinit(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Power down before deinitializing
    tps65185_power_down(handle);
    tps65185_standby(handle);

    // Put device to sleep if WAKEUP pin is configured
    if (handle->gpio_pins.wakeup_pin != GPIO_NUM_NC) {
        gpio_set_level(handle->gpio_pins.wakeup_pin, 0);
    }

    esp_err_t ret = i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return ret;
}

esp_err_t tps65185_get_revid(tps65185_handle_t handle, uint8_t *rev_id)
{
    return tps65185_read_register(handle, TPS65185_REG_REVID, rev_id);
}

/*******************************************************************************
 * Power Control Functions
 ******************************************************************************/

esp_err_t tps65185_wakeup(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->gpio_pins.wakeup_pin != GPIO_NUM_NC) {
        gpio_set_level(handle->gpio_pins.wakeup_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(10));  // Wait for device to wake up
        ESP_LOGD(TAG, "Device woken up via GPIO");
    }

    // Clear standby bit
    return tps65185_modify_register(handle, TPS65185_REG_ENABLE, 
                                     TPS65185_ENABLE_STANDBY, 0);
}

esp_err_t tps65185_standby(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return tps65185_modify_register(handle, TPS65185_REG_ENABLE, 
                                     TPS65185_ENABLE_STANDBY, TPS65185_ENABLE_STANDBY);
}

esp_err_t tps65185_sleep(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Sleep is entered purely through the WAKEUP pin; there is no I2C path
    // (the interface itself powers down), so without the pin we cannot get there.
    if (handle->gpio_pins.wakeup_pin == GPIO_NUM_NC) {
        ESP_LOGD(TAG, "No WAKEUP pin configured, cannot enter sleep");
        return ESP_ERR_NOT_SUPPORTED;
    }

    gpio_set_level(handle->gpio_pins.wakeup_pin, 0);
    ESP_LOGD(TAG, "Device put to sleep via WAKEUP GPIO");
    return ESP_OK;
}

esp_err_t tps65185_power_up(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    uint8_t enable_before, enable_after;

    /*
     * NOTE: the power-up strobe order and delays are NOT set here any more.
     *
     * They are a property of the panel - it dictates the order its source and
     * gate rails must appear in - so they live in the panel definition and are
     * programmed by epd_display_power_on() before each power-up.  This function
     * previously hardcoded one panel's ordering, which silently applied to
     * every panel.
     */

    // Read current ENABLE register state
    tps65185_read_register(handle, TPS65185_REG_ENABLE, &enable_before);

    // Enable all power rails via I2C: V3P3, VNEG, VEE, VPOS, VDDH, VCOM
    // This is required even when using PWRUP GPIO - the GPIO only triggers
    // the power sequence, but rails must be enabled in the ENABLE register first
    uint8_t enable_val = TPS65185_ENABLE_V3P3_EN |
                         TPS65185_ENABLE_VNEG_EN | 
                         TPS65185_ENABLE_VEE_EN | 
                         TPS65185_ENABLE_VPOS_EN | 
                         TPS65185_ENABLE_VDDH_EN |
                         TPS65185_ENABLE_VCOM_EN;

    ret = tps65185_write_register(handle, TPS65185_REG_ENABLE, enable_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write ENABLE register");
        return ret;
    }

    /*
     * The readback normally differs from what was just written, and that is
     * expected rather than a failure: with the PWRUP pin driving the sequencer
     * the rail bits reflect the state machine's own progress, not the request.
     * Logged at debug level so it is available when tracing without implying
     * something is wrong.
     */
    tps65185_read_register(handle, TPS65185_REG_ENABLE, &enable_after);
    ESP_LOGD(TAG, "ENABLE 0x%02X -> wrote 0x%02X -> reads 0x%02X",
             enable_before, enable_val, enable_after);

    // If PWRUP pin is configured, assert it to trigger power sequencing
    if (handle->gpio_pins.pwrup_pin != GPIO_NUM_NC) {
        gpio_set_level(handle->gpio_pins.pwrup_pin, 1);
        ESP_LOGD(TAG, "PWRUP asserted, waiting for rails");
    }

    /*
     * Wait for the rails, rather than sleeping a fixed time and then judging.
     *
     * The rails come up in a programmed sequence with a delay between strobes,
     * so any fixed wait races the sequencer.  Measured on an epdInky board with
     * an ED103TC2: VPOS/VEE/VNEG are good about 150 ms after PWRUP, but VDDH
     * not until roughly 300 ms.  Checking once at 100 ms therefore reported a
     * missing gate rail on a perfectly healthy panel - a warning that is worse
     * than no warning at all, because it teaches you to ignore the one case it
     * exists to catch.
     *
     * Polling costs nothing when the rails are healthy: the loop exits as soon
     * as they are up, so the timeout only applies when something is wrong.
     */
    const int poll_ms    = 10;
    const int timeout_ms = 800;
    int       waited     = 0;
    uint8_t   pg_status  = 0;
    bool      power_good = false;

    /*
     * Judge the rails from the PG register rather than the PWR_GOOD pin.
     *
     * The register names each rail individually, so a failure can say which
     * one is missing, and it does not depend on a board having fitted the
     * pull-up that the open-drain PWR_GOOD output needs.  It is also the same
     * source the EPD driver reports from, so the two can never disagree - the
     * previous mix of pin here and register there produced exactly that,
     * warning about missing rails on the same line that listed them all good.
     */
    const uint8_t rails = TPS65185_PG_VB_PG   | TPS65185_PG_VDDH_PG |
                          TPS65185_PG_VN_PG   | TPS65185_PG_VPOS_PG |
                          TPS65185_PG_VEE_PG  | TPS65185_PG_VNEG_PG;

    for (;;) {
        ret = tps65185_get_power_good_status(handle, &pg_status);
        if (ret != ESP_OK) {
            return ret;
        }
        power_good = ((pg_status & rails) == rails);
        if (power_good || waited >= timeout_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited += poll_ms;
    }

    if (power_good) {
        ESP_LOGI(TAG, "All power rails up and good after %d ms (PG=0x%02X)",
                 waited, pg_status);
        return ESP_OK;
    }

    /* Only now is a complaint justified - and it names the missing rails, so
     * the message says which part of the supply failed rather than just that
     * something did. */
    ESP_LOGW(TAG, "Power rails not good after %d ms: PG=0x%02X "
                  "(VB=%s VDDH=%s VN=%s VPOS=%s VEE=%s VNEG=%s) - check the panel connection",
             timeout_ms, pg_status,
             (pg_status & TPS65185_PG_VB_PG)   ? "ok" : "MISSING",
             (pg_status & TPS65185_PG_VDDH_PG) ? "ok" : "MISSING",
             (pg_status & TPS65185_PG_VN_PG)   ? "ok" : "MISSING",
             (pg_status & TPS65185_PG_VPOS_PG) ? "ok" : "MISSING",
             (pg_status & TPS65185_PG_VEE_PG)  ? "ok" : "MISSING",
             (pg_status & TPS65185_PG_VNEG_PG) ? "ok" : "MISSING");

    return ESP_OK;
}

esp_err_t tps65185_power_down(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    esp_err_t seq_ret = ESP_OK;

    // Configure the power-DOWN sequence so the PMIC ramps the rails off in
    // controlled reverse order instead of collapsing them all at once. Without
    // this the panel is left with residual charge -> grainy pixels after shutdown.
    // DWNSEQ0 = 0x1E: reverse of the 0xE1 up order (VDDH 1st, VPOS 2nd, VNEG 3rd, VEE 4th)
    //
    // NOTE: a failure here must NOT abort the function. Cutting the rails with a
    // sub-optimal ramp is bad; leaving them powered because an I2C write failed
    // is far worse, so the sequence config is best-effort and the actual
    // power-down below always runs.
    ret = tps65185_set_powerdown_sequence(handle, TPS65185_REG_DWNSEQ0,
                                          TPS65185_STROBE_3MS,    // VDDH
                                          TPS65185_STROBE_6MS,    // VPOS
                                          TPS65185_STROBE_12MS,   // VEE
                                          TPS65185_STROBE_9MS);   // VNEG
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set power-down sequence: %s", esp_err_to_name(ret));
        seq_ret = ret;
    }

    // DWNSEQ1 (0x0C): staggered strobe delays + active discharge on power-down
    uint8_t dwnseq1 = 0xE0;
    ESP_LOGI(TAG, "Power-down sequence: reg 0x%02X = 0x%02X", TPS65185_REG_DWNSEQ1, dwnseq1);
    ret = tps65185_write_register(handle, TPS65185_REG_DWNSEQ1, dwnseq1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write DWNSEQ1 register: %s", esp_err_to_name(ret));
        seq_ret = ret;
    }

    // If PWRUP pin is configured, use hardware control. De-asserting PWRUP now
    // triggers the PMIC's internal down-sequencer using the DWNSEQ timing above.
    // This is the mandatory step: it does not depend on I2C, so it still cuts
    // the rails even when the bus is wedged.
    if (handle->gpio_pins.pwrup_pin != GPIO_NUM_NC) {
        gpio_set_level(handle->gpio_pins.pwrup_pin, 0);
        ESP_LOGD(TAG, "Power rails disabled via PWRUP GPIO");
    } else {
        // Use I2C to disable all rails; the DWNSEQ timing still sequences them off
        uint8_t disable_mask = TPS65185_ENABLE_VNEG_EN | 
                               TPS65185_ENABLE_VEE_EN | 
                               TPS65185_ENABLE_VPOS_EN | 
                               TPS65185_ENABLE_VDDH_EN |
                               TPS65185_ENABLE_VCOM_EN;
        
        ret = tps65185_modify_register(handle, TPS65185_REG_ENABLE, disable_mask, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to disable rails via I2C: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGD(TAG, "Power rails disabled via I2C");
    }

    // Wait for the sequenced power-down and active discharge to complete
    vTaskDelay(pdMS_TO_TICKS(50));

    // Rails are down either way; report any sequence-programming failure so the
    // caller knows the ramp may not have been ideal.
    return seq_ret;
}

esp_err_t tps65185_is_power_good(tps65185_handle_t handle, bool *power_good)
{
    if (handle == NULL || power_good == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check PWRGOOD GPIO if available
    if (handle->gpio_pins.pwr_good_pin != GPIO_NUM_NC) {
        *power_good = (gpio_get_level(handle->gpio_pins.pwr_good_pin) == 1);
        return ESP_OK;
    }

    // Otherwise read PG register
    uint8_t pg_status;
    esp_err_t ret = tps65185_read_register(handle, TPS65185_REG_PG, &pg_status);
    if (ret != ESP_OK) {
        return ret;
    }

    // Check if all power good bits are set
    *power_good = (pg_status & TPS65185_PG_ALL) != 0;
    return ESP_OK;
}

esp_err_t tps65185_get_power_good_status(tps65185_handle_t handle, uint8_t *pg_status)
{
    return tps65185_read_register(handle, TPS65185_REG_PG, pg_status);
}

esp_err_t tps65185_set_vpos_vneg(tps65185_handle_t handle, tps65185_vset_t vset)
{
    if (handle == NULL || vset > TPS65185_VSET_12V) {
        return ESP_ERR_INVALID_ARG;
    }

    return tps65185_modify_register(handle, TPS65185_REG_VADJ, 
                                     TPS65185_VADJ_VSET_MASK, vset);
}

/*******************************************************************************
 * VCOM Control Functions
 ******************************************************************************/

esp_err_t tps65185_set_vcom(tps65185_handle_t handle, uint16_t vcom_mv)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clamp VCOM value to valid range
    if (vcom_mv > TPS65185_VCOM_MAX_MV) {
        vcom_mv = TPS65185_VCOM_MAX_MV;
        ESP_LOGW(TAG, "VCOM clamped to maximum: %d mV", TPS65185_VCOM_MAX_MV);
    }

    // Convert mV to register value (9-bit, 10mV steps)
    uint16_t vcom_reg = vcom_mv / TPS65185_VCOM_STEP_MV;

    // Write VCOM1 (bits 7:0)
    esp_err_t ret = tps65185_write_register(handle, TPS65185_REG_VCOM1, (uint8_t)(vcom_reg & 0xFF));
    if (ret != ESP_OK) {
        return ret;
    }

    // Write VCOM2 (bit 8 in bit 0 position, preserve other bits)
    ret = tps65185_modify_register(handle, TPS65185_REG_VCOM2, 
                                    TPS65185_VCOM2_VCOM8, 
                                    (vcom_reg >> 8) & TPS65185_VCOM2_VCOM8);

    ESP_LOGD(TAG, "VCOM set to %d mV (reg value: 0x%03X)", vcom_mv, vcom_reg);
    return ret;
}

esp_err_t tps65185_get_vcom(tps65185_handle_t handle, uint16_t *vcom_mv)
{
    if (handle == NULL || vcom_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t vcom1, vcom2;
    esp_err_t ret = tps65185_read_register(handle, TPS65185_REG_VCOM1, &vcom1);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = tps65185_read_register(handle, TPS65185_REG_VCOM2, &vcom2);
    if (ret != ESP_OK) {
        return ret;
    }

    // Combine 9-bit value and convert to mV
    uint16_t vcom_reg = ((vcom2 & TPS65185_VCOM2_VCOM8) << 8) | vcom1;
    *vcom_mv = vcom_reg * TPS65185_VCOM_STEP_MV;

    return ESP_OK;
}

esp_err_t tps65185_vcom_enable(tps65185_handle_t handle, bool enable)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // If VCOM_CTRL GPIO is configured, use it
    if (handle->gpio_pins.vcom_ctrl_pin != GPIO_NUM_NC) {
        gpio_set_level(handle->gpio_pins.vcom_ctrl_pin, enable ? 1 : 0);
        return ESP_OK;
    }

    // Otherwise use I2C
    return tps65185_modify_register(handle, TPS65185_REG_ENABLE, 
                                     TPS65185_ENABLE_VCOM_EN, 
                                     enable ? TPS65185_ENABLE_VCOM_EN : 0);
}

esp_err_t tps65185_vcom_hiz(tps65185_handle_t handle, bool hiz)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return tps65185_modify_register(handle, TPS65185_REG_VCOM2, 
                                     TPS65185_VCOM2_HIZ, 
                                     hiz ? TPS65185_VCOM2_HIZ : 0);
}

esp_err_t tps65185_vcom_program_nvm(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Programming VCOM to NVM...");

    // Set PROG bit to start programming
    esp_err_t ret = tps65185_modify_register(handle, TPS65185_REG_VCOM2, 
                                              TPS65185_VCOM2_PROG, TPS65185_VCOM2_PROG);
    if (ret != ESP_OK) {
        return ret;
    }

    // Wait for programming to complete (typical 100ms)
    vTaskDelay(pdMS_TO_TICKS(150));

    // Check if programming is complete by reading INT1 for PRGC
    uint8_t int1;
    ret = tps65185_read_register(handle, TPS65185_REG_INT1, &int1);
    if (ret != ESP_OK) {
        return ret;
    }

    if (int1 & TPS65185_INT1_PRGC) {
        ESP_LOGI(TAG, "VCOM programmed to NVM successfully");
    } else {
        ESP_LOGW(TAG, "VCOM NVM programming status unclear");
    }

    return ESP_OK;
}

/*******************************************************************************
 * Temperature Sensing Functions
 ******************************************************************************/

esp_err_t tps65185_read_temperature(tps65185_handle_t handle, int8_t *temperature)
{
    if (handle == NULL || temperature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Trigger thermistor conversion
    esp_err_t ret = tps65185_modify_register(handle, TPS65185_REG_TMST1, 
                                              TPS65185_TMST1_READ_THERM, 
                                              TPS65185_TMST1_READ_THERM);
    if (ret != ESP_OK) {
        return ret;
    }

    // Wait for conversion to complete (typical 10ms)
    vTaskDelay(pdMS_TO_TICKS(15));

    // Read temperature value
    uint8_t raw_temp;
    ret = tps65185_read_register(handle, TPS65185_REG_TMST_VALUE, &raw_temp);
    if (ret != ESP_OK) {
        return ret;
    }

    // The temperature is stored as a signed 8-bit value
    // Temperature range is approximately -20°C to +85°C
    *temperature = (int8_t)raw_temp;

    ESP_LOGD(TAG, "Temperature: %d°C (raw: 0x%02X)", *temperature, raw_temp);
    return ESP_OK;
}

esp_err_t tps65185_read_thermistor_raw(tps65185_handle_t handle, uint8_t *raw_value)
{
    return tps65185_read_register(handle, TPS65185_REG_TMST_VALUE, raw_value);
}

/*******************************************************************************
 * Power Sequencing Functions
 ******************************************************************************/

esp_err_t tps65185_set_powerup_sequence(tps65185_handle_t handle,
                                         uint8_t reg,
                                         tps65185_strobe_t vddh_strobe,
                                         tps65185_strobe_t vpos_strobe,
                                         tps65185_strobe_t vee_strobe,
                                         tps65185_strobe_t vneg_strobe)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Note: In TPS65185, the power-up order in UPSEQ0 is:
    // VEE[7:6], VNEG[5:4], VPOS[3:2], VDDH[1:0]
    uint8_t upseq0 = ((vee_strobe << TPS65185_UPSEQ0_VEE_STROBE_SHIFT) & TPS65185_UPSEQ0_VEE_STROBE_MASK) |
                     ((vneg_strobe << TPS65185_UPSEQ0_VNEG_STROBE_SHIFT) & TPS65185_UPSEQ0_VNEG_STROBE_MASK) |
                     ((vpos_strobe << TPS65185_UPSEQ0_VPOS_STROBE_SHIFT) & TPS65185_UPSEQ0_VPOS_STROBE_MASK) |
                     ((vddh_strobe << TPS65185_UPSEQ0_VDDH_STROBE_SHIFT) & TPS65185_UPSEQ0_VDDH_STROBE_MASK);

    ESP_LOGI(TAG, "Power-up sequence: reg 0x%02X = 0x%02X", reg, upseq0);
    return tps65185_write_register(handle, reg, upseq0);
}

esp_err_t tps65185_set_powerdown_sequence(tps65185_handle_t handle,
                                           uint8_t reg,
                                           tps65185_strobe_t vddh_strobe,
                                           tps65185_strobe_t vpos_strobe,
                                           tps65185_strobe_t vee_strobe,
                                           tps65185_strobe_t vneg_strobe)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Note: In TPS65185, the power-down order in DWNSEQ0 is:
    // VDDH[7:6], VPOS[5:4], VEE[3:2], VNEG[1:0]
    uint8_t dwnseq0 = ((vddh_strobe << TPS65185_DWNSEQ0_VDDH_STROBE_SHIFT) & TPS65185_DWNSEQ0_VDDH_STROBE_MASK) |
                      ((vpos_strobe << TPS65185_DWNSEQ0_VPOS_STROBE_SHIFT) & TPS65185_DWNSEQ0_VPOS_STROBE_MASK) |
                      ((vee_strobe << TPS65185_DWNSEQ0_VEE_STROBE_SHIFT) & TPS65185_DWNSEQ0_VEE_STROBE_MASK) |
                      ((vneg_strobe << TPS65185_DWNSEQ0_VNEG_STROBE_SHIFT) & TPS65185_DWNSEQ0_VNEG_STROBE_MASK);

    ESP_LOGI(TAG, "Power-down sequence: reg 0x%02X = 0x%02X", reg, dwnseq0);
    return tps65185_write_register(handle, reg, dwnseq0);
}

/*******************************************************************************
 * Interrupt Functions
 ******************************************************************************/

esp_err_t tps65185_enable_interrupts(tps65185_handle_t handle, uint8_t int_en1, uint8_t int_en2)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = tps65185_write_register(handle, TPS65185_REG_INT_EN1, int_en1);
    if (ret != ESP_OK) {
        return ret;
    }

    return tps65185_write_register(handle, TPS65185_REG_INT_EN2, int_en2);
}

esp_err_t tps65185_read_interrupts(tps65185_handle_t handle, uint8_t *int1, uint8_t *int2)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    if (int1 != NULL) {
        ret = tps65185_read_register(handle, TPS65185_REG_INT1, int1);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (int2 != NULL) {
        ret = tps65185_read_register(handle, TPS65185_REG_INT2, int2);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

/*******************************************************************************
 * Debug Functions
 ******************************************************************************/

esp_err_t tps65185_dump_registers(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "=== TPS65185 Register Dump ===");

    const struct {
        uint8_t reg;
        const char *name;
    } regs[] = {
        {TPS65185_REG_TMST_VALUE, "TMST_VALUE"},
        {TPS65185_REG_ENABLE, "ENABLE"},
        {TPS65185_REG_VADJ, "VADJ"},
        {TPS65185_REG_VCOM1, "VCOM1"},
        {TPS65185_REG_VCOM2, "VCOM2"},
        {TPS65185_REG_INT_EN1, "INT_EN1"},
        {TPS65185_REG_INT_EN2, "INT_EN2"},
        {TPS65185_REG_INT1, "INT1"},
        {TPS65185_REG_INT2, "INT2"},
        {TPS65185_REG_UPSEQ0, "UPSEQ0"},
        {TPS65185_REG_UPSEQ1, "UPSEQ1"},
        {TPS65185_REG_DWNSEQ0, "DWNSEQ0"},
        {TPS65185_REG_DWNSEQ1, "DWNSEQ1"},
        {TPS65185_REG_TMST1, "TMST1"},
        {TPS65185_REG_TMST2, "TMST2"},
        {TPS65185_REG_PG, "PG"},
        {TPS65185_REG_REVID, "REVID"},
    };

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t value;
        esp_err_t ret = tps65185_read_register(handle, regs[i].reg, &value);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X %-12s: 0x%02X", regs[i].reg, regs[i].name, value);
        } else {
            ESP_LOGE(TAG, "  0x%02X %-12s: READ ERROR", regs[i].reg, regs[i].name);
        }
    }

    ESP_LOGI(TAG, "==============================");
    return ESP_OK;
}

esp_err_t tps65185_vcom_diagnostic(tps65185_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t enable_reg, vcom1, vcom2, vadj;
    
    ESP_LOGI(TAG, "=== TPS65185 VCOM Diagnostic ===");
    
    // Read ENABLE register
    tps65185_read_register(handle, TPS65185_REG_ENABLE, &enable_reg);
    ESP_LOGI(TAG, "ENABLE register: 0x%02X", enable_reg);
    ESP_LOGI(TAG, "  ACTIVE:   %s", (enable_reg & TPS65185_ENABLE_ACTIVE) ? "YES" : "NO");
    ESP_LOGI(TAG, "  STANDBY:  %s", (enable_reg & TPS65185_ENABLE_STANDBY) ? "YES" : "NO");
    ESP_LOGI(TAG, "  V3P3_EN:  %s", (enable_reg & TPS65185_ENABLE_V3P3_EN) ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "  VCOM_EN:  %s", (enable_reg & TPS65185_ENABLE_VCOM_EN) ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "  VDDH_EN:  %s", (enable_reg & TPS65185_ENABLE_VDDH_EN) ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "  VPOS_EN:  %s", (enable_reg & TPS65185_ENABLE_VPOS_EN) ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "  VEE_EN:   %s", (enable_reg & TPS65185_ENABLE_VEE_EN) ? "ENABLED" : "DISABLED");
    ESP_LOGI(TAG, "  VNEG_EN:  %s", (enable_reg & TPS65185_ENABLE_VNEG_EN) ? "ENABLED" : "DISABLED");
    
    // Read VADJ register
    tps65185_read_register(handle, TPS65185_REG_VADJ, &vadj);
    const char* vset_str[] = {"15V", "14V", "13V", "12V"};
    ESP_LOGI(TAG, "VADJ register: 0x%02X (VPOS/VNEG = +/-%s)", vadj, vset_str[vadj & 0x03]);
    
    // Read VCOM registers
    tps65185_read_register(handle, TPS65185_REG_VCOM1, &vcom1);
    tps65185_read_register(handle, TPS65185_REG_VCOM2, &vcom2);
    
    uint16_t vcom_reg = ((vcom2 & TPS65185_VCOM2_VCOM8) << 8) | vcom1;
    uint16_t vcom_mv = vcom_reg * TPS65185_VCOM_STEP_MV;
    
    ESP_LOGI(TAG, "VCOM1: 0x%02X, VCOM2: 0x%02X", vcom1, vcom2);
    ESP_LOGI(TAG, "VCOM setting: -%d.%02dV (register value: %d)", 
             vcom_mv / 1000, (vcom_mv % 1000) / 10, vcom_reg);
    ESP_LOGI(TAG, "  VCOM Hi-Z:  %s", (vcom2 & TPS65185_VCOM2_HIZ) ? "YES (Hi-Z mode!)" : "NO (normal)");
    ESP_LOGI(TAG, "  ACQ bit:    %s", (vcom2 & TPS65185_VCOM2_ACQ) ? "SET" : "CLEAR");
    ESP_LOGI(TAG, "  PROG bit:   %s", (vcom2 & TPS65185_VCOM2_PROG) ? "SET" : "CLEAR");
    
    // Check for VCOM issues
    if (!(enable_reg & TPS65185_ENABLE_VCOM_EN)) {
        ESP_LOGW(TAG, "*** VCOM is NOT ENABLED! Call tps65185_vcom_enable(handle, true) ***");
    }
    
    if (vcom2 & TPS65185_VCOM2_HIZ) {
        ESP_LOGW(TAG, "*** VCOM is in Hi-Z mode! Call tps65185_vcom_hiz(handle, false) ***");
    }
    
    if (vcom_mv == 0) {
        ESP_LOGW(TAG, "*** VCOM is set to 0V - this is unusual for EPD ***");
    }
    
    ESP_LOGI(TAG, "================================");
    
    return ESP_OK;
}
