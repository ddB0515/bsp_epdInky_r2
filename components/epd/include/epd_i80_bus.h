#ifndef EPD_I80_BUS_H
#define EPD_I80_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#if !SOC_LCD_I80_SUPPORTED
#error "The epd component requires a chip with the LCD i80 peripheral (ESP32-S2/S3/P4)."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * EPD parallel source-bus configuration.
 *
 * ioCL (pin_cl) is the source shift-clock.  It is wired to the ESP32-P4 LCD
 * controller's WR strobe and is driven by hardware DMA — not bit-banged.
 *
 * SPV / CKV / SPH / OE / LE are all bit-banged GPIOs that the panel driver
 * sequences around each DMA row transfer.
 *
 * NOTE: ioPWR (GPIO 26) and ioPWR_Good (GPIO 27) are owned by the TPS65185
 *       driver and must NOT appear here.
 */
typedef struct {
    /**
     * Data bus pins D0–D15 in order.
     * For 8-bit bus mode set pins [8..15] to GPIO_NUM_NC.
     */
    gpio_num_t data_pins[16];

    gpio_num_t pin_cl;      /**< ioCL  – source shift-clock  (LCD WR, hw-driven) */
    gpio_num_t pin_le;      /**< ioLE  – source latch-enable                      */
    gpio_num_t pin_oe;      /**< ioOE  – output-enable (polarity: oe_active_high) */
    gpio_num_t pin_sph;     /**< ioSPH – source start pulse (driven as hardware CS) */
    gpio_num_t pin_spv;     /**< ioSPV – gate start pulse                         */
    gpio_num_t pin_ckv;     /**< ioCKV – gate clock                               */

    /**
     * ioGMOD / EP_MODE – gate driver mode enable.
     *
     * Asserted while the panel is powered and driven, de-asserted to reset the
     * gate driver during power-down.  Set to GPIO_NUM_NC on boards that tie
     * this signal high in hardware.
     */
    gpio_num_t pin_gmod;

    /**
     * Polarity of pin_oe.
     *
     * true  → driving the pin HIGH enables the source outputs.
     * false → driving the pin LOW  enables the source outputs (classic
     *         active-low /OE).
     *
     * Boards differ here, so this is data rather than a naming convention:
     * always call epd_i80_oe_enable() / epd_i80_oe_disable() and let this
     * flag resolve the electrical level.
     */
    bool oe_active_high;

    /**
     * Dummy DC pin required by the ESP32-P4 LCD i80 peripheral (IDF v6.0+).
     * The LCD driver registers this GPIO to the DC peripheral signal but,
     * since lcd_cmd_bits=0, the DC line is never toggled during pixel
     * transfers.  Assign any free GPIO that is not connected to the panel.
     * EPD panels have no DC line.
     */
    gpio_num_t pin_dc_dummy;

    /** Pixel clock for the i80 bus in Hz.  Typical EPD range: 4–20 MHz. */
    uint32_t pclk_hz;

    /**
     * Largest single DMA transfer in bytes.
     * Must be >= width_px * (bus_width / 8).
     * Buffer must be allocated from internal SRAM or PSRAM with DMA capability.
     */
    size_t max_row_bytes;
} epd_i80_bus_config_t;

typedef struct epd_i80_bus_dev *epd_i80_bus_handle_t;

/**
 * @brief  Initialise the EPD i80 bus (LCD peripheral + GPIO).
 *
 * @param config       Bus pin and clock configuration.
 * @param bus_width    Data-bus width: 8 or 16.
 * @param handle       Output handle.
 * @return esp_err_t   ESP_OK on success.
 */
esp_err_t epd_i80_bus_init(const epd_i80_bus_config_t *config,
                            uint8_t bus_width,
                            epd_i80_bus_handle_t *handle);

/**
 * @brief  Release all resources acquired by epd_i80_bus_init().
 */
esp_err_t epd_i80_bus_deinit(epd_i80_bus_handle_t handle);

/**
 * @brief  Queue one row of pixel data for DMA and return immediately.
 *
 * The transfer is still in flight when this returns, so @p row_data must stay
 * valid and unmodified until the matching epd_i80_bus_wait_row() completes.
 * Use two alternating row buffers to build the next row while this one is on
 * the wire.
 *
 * Exactly one transfer may be outstanding at a time.  The bus mutex is
 * ACQUIRED here and RELEASED by epd_i80_bus_wait_row(), so the two calls MUST
 * be paired from the same task — epd_i80_bus_wait_row() rejects a caller that
 * did not submit.  Other tasks are locked out for the whole submit/wait pair.
 * Must NOT be called from an ISR, nor from inside
 * epd_i80_bus_enter_critical().
 *
 * @param handle      Bus handle.
 * @param row_data    Pixel data; must be DMA-capable and remain valid until
 *                    epd_i80_bus_wait_row() returns.
 * @param len_bytes   Byte count: width_px * (bus_width / 8).
 */
esp_err_t epd_i80_bus_send_row_async(epd_i80_bus_handle_t handle,
                                      const void *row_data,
                                      size_t len_bytes);

/**
 * @brief  Block until the transfer queued by epd_i80_bus_send_row_async()
 *         has completed, then release the bus.
 *
 * Returns ESP_OK immediately when no transfer is outstanding, so it is safe to
 * call as a "drain" on error paths.  Must be called from the same task that
 * called epd_i80_bus_send_row_async(); any other caller gets
 * ESP_ERR_INVALID_STATE and the transfer is left in flight.
 */
esp_err_t epd_i80_bus_wait_row(epd_i80_bus_handle_t handle);

/**
 * @brief  esp_timer timestamp (µs) at which the last row transfer's completion
 *         interrupt fired.
 *
 * Recorded inside the ISR, so subtracting it from the time the waiting task
 * actually resumed isolates scheduler wake latency from real DMA time.
 * Intended for driver instrumentation only.
 */
int64_t epd_i80_bus_last_done_us(epd_i80_bus_handle_t handle);

/**
 * @brief  DMA-transfer one row of pixel data via the source bus.
 *
 * Convenience wrapper: epd_i80_bus_send_row_async() followed immediately by
 * epd_i80_bus_wait_row().  Blocks until the DMA completion callback fires.
 * The caller must manage gate-timing signals (SPV / CKV / OE / LE) around
 * this call.
 *
 * Prefer the async pair on the per-row hot path — blocking here costs a full
 * task block/unblock round-trip per row and makes the row period depend on
 * scheduler latency.
 *
 * @param handle      Bus handle.
 * @param row_data    Pixel data; must be DMA-capable and remain valid until
 *                    the function returns.
 * @param len_bytes   Byte count: width_px * (bus_width / 8).
 */
esp_err_t epd_i80_bus_send_row(epd_i80_bus_handle_t handle,
                                const void *row_data,
                                size_t len_bytes);

/* ── Gate / control signal primitives (bit-bang) ─────────────────────────── */
/* Inline wrappers so panel drivers read as natural signal names.             */

void epd_i80_ckv_high(epd_i80_bus_handle_t handle);
void epd_i80_ckv_low(epd_i80_bus_handle_t handle);
void epd_i80_spv_high(epd_i80_bus_handle_t handle);
void epd_i80_spv_low(epd_i80_bus_handle_t handle);
void epd_i80_le_high(epd_i80_bus_handle_t handle);
void epd_i80_le_low(epd_i80_bus_handle_t handle);

/*
 * NOTE: there are deliberately no SPH helpers.  SPH is assigned to the LCD
 * i80 peripheral as hardware CS and is driven automatically around every DMA
 * transfer; bit-banging it from software would fight the peripheral.
 */

/** Enable the source driver outputs (resolves pin_oe via oe_active_high). */
void epd_i80_oe_enable(epd_i80_bus_handle_t handle);

/** Disable / tri-state the source driver outputs. */
void epd_i80_oe_disable(epd_i80_bus_handle_t handle);

/** Drive the gate-driver mode pin (no-op when pin_gmod is GPIO_NUM_NC). */
void epd_i80_gmod_set(epd_i80_bus_handle_t handle, bool enable);

/* ── Timing-critical section ─────────────────────────────────────────────── */

/**
 * @brief  Enter a spinlock-protected, interrupt-disabled region.
 *
 * Bit-banged gate pulse trains (CKV / SPV / LE) are timing sensitive: a
 * preempting ISR between two gpio_set_level() calls stretches the pulse and
 * can shift a gate row, producing banding.  Wrap short pulse sequences in
 * these calls to make them atomic.
 *
 * WARNING: interrupts are disabled inside this region, so it must be kept
 * short and must NEVER contain epd_i80_bus_send_row() — that call blocks on a
 * semaphore given from the DMA completion ISR and would deadlock instantly.
 */
void epd_i80_bus_enter_critical(epd_i80_bus_handle_t handle);
void epd_i80_bus_exit_critical(epd_i80_bus_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* EPD_I80_BUS_H */
