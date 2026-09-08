# epdInky ESP32-P4 — Arduino + FastEPD

An Arduino sketch that draws a status screen on the 10.3" ED103TC2 panel, using
`setup()` / `loop()` and the FastEPD library.

```
idf.py set-target esp32p4
idf.py build flash monitor
```

Expect a clear to white, the screen at about 2.3 s, then a redraw every 30 s
with a live uptime and heap counter.

## What the sketch does

FastEPD owns the e-paper side of the board completely: the 16-bit parallel bus,
the row and gate timing, the greyscale waveforms and the TPS65185 PMIC. The
sketch only says which panel is fitted and draws.

`BB_PANEL_EPDINKY_P4_16` is FastEPD's own definition for this board and its pin
map matches the schematic, so no pins are described by hand. It deliberately
carries no panel size — the board is a carrier and the glass varies — so
`setPanelSize()` must follow `initPanel()`; that call is what allocates the
frame buffers and builds the lookup tables.

## Four things that had to be got right

**`Serial` must not be UART0.** On this board UART0 is GPIO37/GPIO38, the
TPS65185 WAKEUP and INT lines, so Arduino's default `Serial` would drive the
PMIC control lines as a UART. The top-level `CMakeLists.txt` sets
`ARDUINO_USB_CDC_ON_BOOT=1` so `Serial` is the USB Serial/JTAG device.

**`ARDUINO` has to be defined build-wide.** FastEPD's header derives `FASTEPD`
from `Print` when `ARDUINO` is defined and not otherwise, which changes whether
the class has a vtable. Compiling the sketch as Arduino while the FastEPD
component built the plain version fails at link time with `undefined reference
to vtable for FASTEPD`. Setting the define on the build-wide property keeps both
on the same class definition.

**FastEPD then needs the Arduino headers.** Once `ARDUINO` is defined it
includes `Wire.h`, which drags in the whole core. The project CMakeLists links
the FastEPD component against arduino-esp32's include set rather than editing
the managed component, which a dependency update would overwrite.

**FastEPD owns I2C, so the BSP is not initialised.** This is the one that
produces a working-looking build that fails on hardware. The IDF build of
FastEPD adopts an existing bus — `bbepI2CInit()` tries
`i2c_master_get_bus_handle(I2C_NUM_0)` first — which is why the IDF example can
bring the BSP up and share. The Arduino build takes a different path in
`arduino_io.inl`: `Wire.end()` then `Wire.begin()`, always creating its own bus.
On a port the BSP already holds that gives

```
i2c_new_master_bus failed: ESP_ERR_INVALID_STATE
clearWhite failed: I/O error
```

and every PMIC access afterwards fails. A sketch that needs the other board
devices would have to reach them over `Wire` rather than through the C BSP.

## VCOM

`EPD_VCOM_MV` is printed on the panel's own flexible cable and belongs to the
glass, not the board. A wrong value degrades the image — washed out, or heavy
ghosting — rather than producing an error, so it fails quietly. Change it and
the panel size together if different glass is fitted.

## 1bpp, not greyscale

`setMode(BB_MODE_1BPP)` is deliberate: the 4bpp greyscale mode needs a waveform
this panel does not ship and renders blank. Greys come out as dithered patterns
rather than true tones.
