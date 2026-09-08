#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

/*
 * epdInky ESP32-P4/C6 rev.2 board pin map.
 *
 * All assignments below were verified against the KiCad netlist exported from
 * epdInkyP4C6.kicad_sch (rev.2). Net names in comments match the schematic.
 */

/* ---------------------------------------------------------------------------
 * I2C bus (shared by every on-board device)
 * Nets: I2C_SDA / I2C_SCL
 * NOTE: R34/R38 provide external 2K2 pull-ups, so the internal pull-ups must
 *       stay disabled.
 * ------------------------------------------------------------------------- */
#define BSP_I2C_SCL_IO              GPIO_NUM_29
#define BSP_I2C_SDA_IO              GPIO_NUM_28
#define BSP_I2C_NUM                 I2C_NUM_0
#define BSP_I2C_FREQ_HZ             400000
#define BSP_I2C_TIMEOUT_MS          1000
#define BSP_I2C_INTERNAL_PULLUPS    false

/* Legacy aliases (kept so existing sketches keep compiling) */
#define I2C_MASTER_SCL_IO           BSP_I2C_SCL_IO
#define I2C_MASTER_SDA_IO           BSP_I2C_SDA_IO
#define I2C_MASTER_NUM              BSP_I2C_NUM
#define I2C_MASTER_FREQ_HZ          BSP_I2C_FREQ_HZ
#define I2C_MASTER_TIMEOUT_MS       BSP_I2C_TIMEOUT_MS

/* ---------------------------------------------------------------------------
 * I2C device addresses
 * ------------------------------------------------------------------------- */
/* KXTJ3-1057: ADDR is strapped by jumper JP2 (pin1=3V3, pin3=GND).
 * ADDR high -> 0x0F, ADDR low -> 0x0E. Probe both. */
#define I2C_DEVICE_KXTJ3_ADDR       0x0F
#define I2C_DEVICE_KXTJ3_ADDR_ALT   0x0E
#define I2C_DEVICE_RV3028_ADDR      0x52  /* RV-3028-C7 RTC          */
#define I2C_DEVICE_TPS65185_ADDR    0x68  /* TPS651851 e-Ink PMIC    */
#define I2C_DEVICE_TCA6408_ADDR     0x21  /* ADDR tied to VCC3V3     */
#define I2C_DEVICE_STC3115_ADDR     0x70  /* STC3115 fuel gauge      */

/* ---------------------------------------------------------------------------
 * EPD 16-bit parallel data bus. Nets D0..D15, contiguous on GPIO2..GPIO17.
 * Also broken out on header J4.
 * ------------------------------------------------------------------------- */
#define BSP_EPD_DATA_PINS_DEFAULT   { \
	GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, \
	GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9, \
	GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, \
	GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17  \
}
#define BSP_EPD_DATA_PIN_FIRST      GPIO_NUM_2
#define BSP_EPD_DATA_PIN_COUNT      16

/* EPD control signals (also on header J2) */
#define BSP_EPD_PIN_SPV             GPIO_NUM_45   /* SPV  - vertical start pulse */
#define BSP_EPD_PIN_XSTL            GPIO_NUM_46   /* XSTL - horizontal start (SPH) */
#define BSP_EPD_PIN_XOE             GPIO_NUM_47   /* XOE  - source output enable */
#define BSP_EPD_PIN_XLE             GPIO_NUM_48   /* XLE  - source latch enable */
#define BSP_EPD_PIN_XCL             GPIO_NUM_50   /* XCL  - source shift clock */
#define BSP_EPD_PIN_CKV             GPIO_NUM_51   /* CKV  - gate clock */
#define BSP_EPD_PIN_MODE            GPIO_NUM_52   /* MODE - gate driver mode */

/* BORDER (U5.23) is strapped through jumper JP1 to GND - not software controlled. */

/* ---------------------------------------------------------------------------
 * micro-SD card (SDMMC slot 0), 4-bit. Connector P1.
 * Card power is gated by Q4 (AO3407 P-FET) driven from TCA6408 P7, so the
 * expander must be initialised before the card can be powered.
 * SD1-CD (card detect) is NOT routed to the MCU - do not rely on it.
 * ------------------------------------------------------------------------- */
#define BSP_SD_PIN_CMD              GPIO_NUM_44
#define BSP_SD_PIN_CLK              GPIO_NUM_43
#define BSP_SD_PIN_D0               GPIO_NUM_39
#define BSP_SD_PIN_D1               GPIO_NUM_40
#define BSP_SD_PIN_D2               GPIO_NUM_41
#define BSP_SD_PIN_D3               GPIO_NUM_42
#define BSP_SD_SLOT                 0
#define BSP_SD_MOUNT_POINT          "/sdcard"

/* ---------------------------------------------------------------------------
 * ESP32-C6-MINI-1 companion radio, SDIO slot 1 (used by esp-hosted).
 *
 * IMPORTANT: C6_CHIP_PU (GPIO54) is the C6 EN pin and has a 10K pull-up (R43),
 * so the C6 is enabled by default. esp-hosted owns this pin and drives the
 * HIGH->LOW->HIGH reset pulse itself (CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y).
 * Do NOT drive it low from application code or the radio stays in reset.
 * ------------------------------------------------------------------------- */
#define BSP_C6_WIFI_PIN_CHIP_PU     GPIO_NUM_54  /* C6 EN - owned by esp-hosted */
#define BSP_C6_WIFI_PIN_IO2         GPIO_NUM_53  /* C6_IO2, C6 boot strap       */
#define BSP_C6_WIFI_PIN_CMD         GPIO_NUM_19  /* -> C6 GPIO18 */
#define BSP_C6_WIFI_PIN_CLK         GPIO_NUM_18  /* -> C6 GPIO19 */
#define BSP_C6_WIFI_PIN_D0          GPIO_NUM_23  /* -> C6 GPIO20 */
#define BSP_C6_WIFI_PIN_D1          GPIO_NUM_22  /* -> C6 GPIO21 */
#define BSP_C6_WIFI_PIN_D2          GPIO_NUM_21  /* -> C6 GPIO22 */
#define BSP_C6_WIFI_PIN_D3          GPIO_NUM_20  /* -> C6 GPIO23 */
#define BSP_C6_WIFI_SDIO_SLOT       1

/* ---------------------------------------------------------------------------
 * TCA6408A GPIO expander (addr 0x21).
 * Its open-drain INT (active low, 10K pull-up R52) is the ONLY interrupt line
 * from the accelerometer and RTC back to the P4.
 * ------------------------------------------------------------------------- */
#define BSP_TCA6408_PIN_INT_IO      GPIO_NUM_34  /* GPIOE_INT, active low */

/* Expander port bit indices (0-7), NOT GPIO numbers */
#define BSP_TCA6408_PIN_GSENSOR_INT 0   /* <- KXTJ3 INT   (input)  */
#define BSP_TCA6408_PIN_RTC_INT     1   /* <- RV3028 INT  (input)  */
#define BSP_TCA6408_PIN_EXP_P2      2   /* header J3 pin 4         */
#define BSP_TCA6408_PIN_EXP_P3      3   /* header J3 pin 3         */
#define BSP_TCA6408_PIN_EXP_P4      4   /* header J3 pin 2         */
#define BSP_TCA6408_PIN_EXP_P5      5   /* header J3 pin 1         */
#define BSP_TCA6408_PIN_TP9         6   /* test point TP9          */
#define BSP_TCA6408_PIN_SD_EN       7   /* -> Q4 gate, ACTIVE LOW  */

/* Direction mask for tca6408_set_config(): 1 = input, 0 = output.
 * P0/P1 inputs (interrupts), P2..P5 inputs (safe default for the header),
 * P6 input, P7 output (SD power gate). */
#define BSP_TCA6408_DIR_DEFAULT     0x7F
/* Output register default: SD_EN high = card powered OFF. */
#define BSP_TCA6408_OUT_DEFAULT     0x80

#define BSP_SD_EN_ACTIVE_LEVEL      0   /* drive P7 low to power the card */

/* Legacy aliases */
#define BSP_TCA6408_INT_GPIO_INT    BSP_TCA6408_PIN_INT_IO
#define BSP_TCA6408_INT_GPIO_P7     BSP_TCA6408_PIN_SD_EN
#define BSP_TCA6408_INT_GPIO_P6     BSP_TCA6408_PIN_TP9
#define BSP_TCA6408_INT_GPIO_P5     BSP_TCA6408_PIN_EXP_P5
#define BSP_TCA6408_INT_GPIO_P4     BSP_TCA6408_PIN_EXP_P4
#define BSP_TCA6408_INT_GPIO_P3     BSP_TCA6408_PIN_EXP_P3
#define BSP_TCA6408_INT_GPIO_P2     BSP_TCA6408_PIN_EXP_P2
#define BSP_TCA6408_INT_GPIO_P1     BSP_TCA6408_PIN_RTC_INT
#define BSP_TCA6408_INT_GPIO_P0     BSP_TCA6408_PIN_GSENSOR_INT

/* ---------------------------------------------------------------------------
 * TPS651851 e-Ink PMIC (addr 0x68)
 * ------------------------------------------------------------------------- */
#define BSP_TPS65185_PIN_POWER_UP   GPIO_NUM_26  /* TPS_PWRUP     (output) */
#define BSP_TPS65185_PIN_PWRGOOD    GPIO_NUM_27  /* TPS_PWR_GOOD  (input)  */
#define BSP_TPS65185_PIN_WAKE_UP    GPIO_NUM_37  /* TPS_WAKEUP    (output) */
#define BSP_TPS65185_PIN_INT        GPIO_NUM_38  /* TPS_INT       (input, active low) */
#define BSP_TPS65185_PIN_VCOM_CTRL  GPIO_NUM_49  /* VCOM_CTRL     (output, 100K pull-down R10) */

/* ---------------------------------------------------------------------------
 * MIPI CSI camera connector (U11, 17-pin FPC)
 *
 * 2 data lanes + clock go straight to the P4's CSI pins; the sensor is
 * configured over the shared I2C bus (SCCB) on connector pins 13/14, and takes
 * its 3V3 from pin 15.
 *
 * CSI_IO0/CSI_IO1 are general-purpose sensor control lines. Their meaning is
 * set by whichever camera module is fitted - typically reset, power-down or
 * an externally supplied XCLK. For the SC2336 module, CSI_IO0 is the reset.
 * ------------------------------------------------------------------------- */
#define BSP_CSI_PIN_IO0             GPIO_NUM_32  /* connector pin 11 */
#define BSP_CSI_PIN_IO1             GPIO_NUM_33  /* connector pin 12 */
#define BSP_CSI_PIN_RESET           BSP_CSI_PIN_IO0
#define BSP_CSI_PIN_PWDN            GPIO_NUM_NC  /* not routed on this board */
#define BSP_CSI_DATA_LANES          2

/* ---------------------------------------------------------------------------
 * MIPI-DSI display (connector FPC1, via the D320C2403V1 adapter board)
 *
 * [HW] Verified from both netlists. The panel is a D320C2403V1-MIPI: 3.2",
 * 1024x768, JD9168 driver, GT967 capacitive touch.
 *
 * The mainboard connector carries only the DSI lanes, I2C, and two touch
 * signals; everything else lives on the adapter:
 *
 *   pin 1      VCC3V3
 *   pin 2/3    I2C_SDA / I2C_SCL   - the shared board I2C bus
 *   pin 5      TP_INT   -> GPIO30  - touch interrupt
 *   pin 6      TP_RST   -> GPIO31  - touch reset
 *   pins 14-21 MIPI0 clock and data lanes 0/1
 *
 * The adapter adds a SECOND TCA6408 at 0x20 (the mainboard's is 0x21), and it
 * gates everything the panel needs:
 *
 *   P0  GPIO_EN  -> AP2281 load switch EN  -> LCD_VDD
 *   P1  LCD_RES  -> JD9168 reset
 *   P2  BL_EN    -> SGM37604A HWEN
 *
 * That ordering matters: the backlight controller does not answer on I2C at
 * all until P2 is high, and the panel is unpowered until P0 is high. A scan
 * before those are driven shows neither 0x36 nor 0x5D, which looks exactly
 * like a disconnected display.
 * ------------------------------------------------------------------------- */
#define BSP_DSI_PIN_TOUCH_INT       GPIO_NUM_30  /* FPC1 pin 5 */
#define BSP_DSI_PIN_TOUCH_RST       GPIO_NUM_31  /* FPC1 pin 6 */
#define BSP_DSI_PIN_PANEL_RST       GPIO_NUM_NC  /* driven by the adapter expander */
#define BSP_DSI_DATA_LANES          2

#define BSP_DSI_LCD_H_RES           1024
#define BSP_DSI_LCD_V_RES            768
/*
 * DSI timings - do not "correct" these to the vendor macro's values.
 *
 * The JD9168 vendor header ships 900 Mbps / 50 MHz, but this panel does not
 * lock onto real video at those rates: the DSI bridge never scans the frame
 * buffer out and no VSYNC is generated. It is a confusing failure because the
 * DSI test pattern still displays perfectly - that is produced in the host,
 * downstream of the bridge, so it proves nothing about the data path.
 *
 * 800 Mbps / 45 MHz is what the panel actually works at, and matches the
 * configuration verified on this hardware. 1046 x 796 total pixels at 45 MHz
 * gives roughly 54 Hz.
 */
#define BSP_DSI_LCD_DPI_CLK_MHZ       45
#define BSP_DSI_LANE_BITRATE_MBPS    800

/* Adapter-board expander (TCA6408 #2). Distinct from the mainboard's 0x21. */
#define BSP_I2C_ADDR_LCD_EXPANDER   0x20
#define BSP_LCD_EXP_PIN_POWER_EN       0   /* P0 -> AP2281 EN, panel 3V3   */
#define BSP_LCD_EXP_PIN_PANEL_RST      1   /* P1 -> JD9168 reset, active low */
#define BSP_LCD_EXP_PIN_BACKLIGHT_EN   2   /* P2 -> SGM37604A HWEN         */

#define BSP_I2C_ADDR_BACKLIGHT      0x36  /* SGM37604A on the adapter */
#define BSP_I2C_ADDR_TOUCH          0x5D  /* GT967, address selected by INT at reset */

/* ---------------------------------------------------------------------------
 * User I/O
 * ------------------------------------------------------------------------- */
#define BSP_BUTTON_PIN              GPIO_NUM_35  /* SW4, active low, 10K pull-up R45 */
#define BSP_BUTTON_ACTIVE_LEVEL     0
/* SW3 drives CHIP_PU (hardware reset) and is not visible to software. */

/* ---------------------------------------------------------------------------
 * Devices with no interrupt path to the MCU (poll only):
 *   STC3115 ALM  -> pulled to VBAT, not routed
 *   RV3028 CLKOUT-> test point TP4 only
 *   RV3028 EVI   -> pulled up, not routed
 * ------------------------------------------------------------------------- */
