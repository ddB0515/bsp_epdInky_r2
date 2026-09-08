#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The LED driver lives on the display module, on the board's shared I2C bus. */
#define SGM37604A_I2C_ADDR       0x36
#define SGM37604A_I2C_SPEED_HZ   400000

/* Brightness is 12-bit: low nibble in the LSB register, top 8 bits in the MSB. */
#define SGM37604A_MAX_BRIGHTNESS 4095

#define SGM37604A_REG_LED_ENABLE      0x10
#define SGM37604A_REG_MODE            0x11
#define SGM37604A_REG_BRIGHTNESS_MSB  0x19
#define SGM37604A_REG_BRIGHTNESS_LSB  0x1A
#define SGM37604A_REG_CURRENT         0x1B
#define SGM37604A_REG_FAULT_FLAGS     0x1F

/** @brief Full-scale LED current, which caps the maximum brightness. */
typedef enum {
    SGM37604A_CURRENT_25MA = 0x00,
    SGM37604A_CURRENT_30MA = 0x01,
    SGM37604A_CURRENT_35MA = 0x02,
    SGM37604A_CURRENT_40MA = 0x03,
} sgm37604a_current_t;

/**
 * @brief Attach to the backlight controller and configure it.
 *
 * Probes first, so a disconnected display reports an error rather than a
 * string of failed writes. The backlight is left at zero: the caller raises it
 * once there is something to show, which avoids a bright flash at startup.
 */
esp_err_t sgm37604a_init(i2c_master_bus_handle_t bus, sgm37604a_current_t max_current);

/** @brief Set brightness, 0 to SGM37604A_MAX_BRIGHTNESS. */
esp_err_t sgm37604a_set_brightness(uint16_t level);

/** @brief Set brightness as a percentage, 0 to 100. */
esp_err_t sgm37604a_set_brightness_percent(uint8_t percent);

/** @brief Turn the LED string on or off without changing the brightness. */
esp_err_t sgm37604a_enable(bool on);

/**
 * @brief Read the fault flags register.
 *
 * Worth checking during bring-up: an open or shorted LED string reports here,
 * which distinguishes "the panel is dark because the backlight is broken" from
 * "the panel is lit but the image is wrong". 0 means no faults.
 */
esp_err_t sgm37604a_get_faults(uint8_t *flags);

/** @brief Turn the backlight off and release the I2C device. */
esp_err_t sgm37604a_deinit(void);

#ifdef __cplusplus
}
#endif
