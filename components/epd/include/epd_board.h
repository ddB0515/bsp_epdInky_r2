#ifndef EPD_BOARD_H
#define EPD_BOARD_H

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Board wiring
 *
 * How the panel connector is wired to this particular board.  Deliberately
 * separate from epd_panel_def_t: swap the panel on one board and these pins are
 * unchanged, move the same panel to another board and they all differ.
 *
 * The application owns this - typically filled in from a BSP's pin macros.
 ******************************************************************************/

/** Number of entries in epd_board_config_t::data. */
#define EPD_BOARD_DATA_PINS  16

typedef struct {
    /**
     * Panel source data lines D0..D15, in the PANEL'S OWN NUMBERING.
     *
     * data[0] is the line the panel calls D0.  An 8-bit panel uses entries
     * 0..7 only and the rest may be left as GPIO_NUM_NC.
     *
     * Note this is NOT the order the LCD peripheral wants.  The peripheral
     * always emits the first byte of a transfer on its data signals 0..7, so
     * the two orders differ for a 16-bit bus; the driver applies that mapping
     * for you, which is the point of expressing it this way.  Getting it wrong
     * by hand is a hard fault to diagnose because it produces a completely
     * blank panel rather than a corrupted image - see bus16_low_byte_first.
     */
    gpio_num_t data[EPD_BOARD_DATA_PINS];

    gpio_num_t cl;    /**< XCL  - source shift clock (driven by the peripheral) */
    gpio_num_t le;    /**< XLE  - source latch enable                           */
    gpio_num_t oe;    /**< XOE  - source output enable, polarity below          */
    gpio_num_t sph;   /**< XSTL - source start pulse (driven as hardware CS)    */
    gpio_num_t spv;   /**< SPV / STV - gate start pulse                         */
    gpio_num_t ckv;   /**< CKV  - gate clock                                    */

    /**
     * MODE / EP_MODE - gate driver mode enable.
     * GPIO_NUM_NC on boards that tie this signal high in hardware.
     */
    gpio_num_t gmod;

    /**
     * A spare GPIO for the LCD peripheral's DC signal.
     *
     * An EPD has no command/data line, but the i80 peripheral requires a valid
     * DC pin (IDF rejects a negative value) and routes the signal out through
     * the GPIO matrix.  Because lcd_cmd_bits is 0 the line never toggles - it
     * simply idles high - but it IS driven, so:
     *
     *   Pick a GPIO that is genuinely unconnected on your board.  Pointing it
     *   at a pin some other device drives creates contention.  An input such as
     *   a PMIC interrupt line is a particularly easy mistake to make, since the
     *   symptom is only a lost interrupt plus the two drivers fighting.
     */
    gpio_num_t dc_dummy;

    /**
     * Polarity of oe.  true = HIGH enables the source outputs.
     */
    bool oe_active_high;

    /**
     * 16-bit bus only: which half of the connector takes the first byte.
     *
     * false (default) sends the first byte of each transfer to data[8..15] and
     * the second to data[0..7].  This is what every panel validated with this
     * driver expects.  Set true for the opposite mapping.
     *
     * If a 16-bit panel produces a mirrored or scrambled image that
     * EPD_PANEL_FLAG_MIRROR_X does not fix, try flipping this.
     */
    bool bus16_low_byte_first;
} epd_board_config_t;

#ifdef __cplusplus
}
#endif

#endif /* EPD_BOARD_H */
