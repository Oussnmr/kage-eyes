# Display ghosting diagnosis

## Evidence and scope

Baseline: main `963399e244673883f91fb010c1ef95fb031f6c33`.
Its successful Actions run 35404801020 resolved ESP-IDF 5.5.5, Waveshare BSP 2.0.3,
esp_lvgl_port 2.9.0, LVGL 9.6.0~1 and CO5300 2.2.0. These display component
versions are retained and pinned for this investigation.

The user's board log shows `setup_dma_priv_buffer: Failed to allocate priv TX
buffer`, followed by `panel_io_spi_tx_color: spi transmit (queue) color failed`
and `panel_co5300_draw_bitmap: send color data failed`. This proves that display
updates are being dropped; previous panel pixels remain where the new update
was not transferred. It is not evidence of multiple firmware images.

## Source-level defects

1. BSP 2.0.3 creates a SPI/QSPI CO5300 panel, but registers it with
   `lvgl_port_add_disp_rgb`. In esp_lvgl_port 2.9.0 the RGB partial-refresh path
   calls `lv_disp_flush_ready` immediately after `esp_lcd_panel_draw_bitmap`.
   SPI color transfers are queued, not completed at that point. The same
   rotation buffer can be rewritten before transmission finishes. Furthermore,
   RGB registration invokes `esp_lcd_rgb_panel_register_event_callbacks` with
   a CO5300 object. IDF casts that handle to its RGB implementation and writes
   RGB callback fields: the object types do not match. The resulting board-side
   corruption, if any, was not measured; the invalid API pairing is verified.
2. The BSP allocates 368 * 100 * 2 = 73,600 bytes for each draw/rotation buffer
   using default heap capabilities (`buff_dma = false`). Under this project's
   PSRAM malloc configuration these large allocations can live in external RAM.
   SPI must then obtain an internal DMA staging copy for a color transfer.
   The logs prove those allocations fail on the board. No heap trace is available
   to distinguish fragmentation, low free internal memory, or corruption as the
   immediate allocator state. Microphone initialization adds memory pressure;
   it is not the sole affected application.

Both faults must be corrected together: merely enabling DMA buffers while
retaining immediate RGB flush completion would expose buffer reuse during DMA.
Merely selecting SPI leaves the observed staging-allocation failure possible;
the port also does not handle a failed draw submission by completing the flush.

## Fix

Use a local, licensed copy of BSP 2.0.3 with only the display registration and
DMA allocation flag changed. Select `lvgl_port_add_disp`, which registers the
SPI `on_color_trans_done` callback. Reserve a draw buffer and a software rotation
buffer in DMA-capable memory at startup, each 23,552 bytes (32 native rows), for
47,104 bytes total, excluding allocator overhead. This removes large temporary
color-buffer allocations from normal refreshes and bounds permanent internal
RAM use. A smaller buffer means more transfers; frame rate needs board testing.

No application UI file is changed. Fade/slide transitions, bubble animation,
eyes, touch, sleep/wake, angry taps, shake, 90/270 degree rotation, microphone
and Wi-Fi Improv are retained. The previously proposed `fe06eeb` transition
workaround is not cherry-picked, so it cannot hide the transport issue.
No remote-control branch is merged or changed.

## Other paths inspected

- All application root screens have opaque backgrounds. Micro, Motion and
  System timer callbacks already check the active screen; eyes pause their timer.
- Home continues updating off-screen. This is avoidable work, but not proof of
  cross-screen painting; changing it is unnecessary for the transport fix.
- LVGL mutations in application timers run in LVGL context; orientation changes
  use the BSP lock. The old SH8601/main.c path is not in main's compiled sources.
- BSP even-coordinate invalidation rounding is retained, as are software
  rotation and byte swapping. Landscape 90/270 degrees keep the UI at 448x368.
- Objects beyond their parent's bounds do not explain SPI allocation errors.
- The simulator in sim/ renders the older tank application, not this BSP/UI;
  it cannot validate this board's DMA behavior.

## Validation and limits

A successful firmware build proves compile/link compatibility, not a hardware
fix. Board validation remains mandatory before merge. The dropped-transfer
mechanism is supported by the supplied logs and exact sources; the contribution
of premature buffer reuse versus allocation failure to each visible artifact
has not been separately measured.

Flash the branch artifact, not the main Pages installer. No erase is needed.
Keep the serial log open and perform the following sequence at least 10 times:

1. Home -> Micro (speak for 10 seconds) -> Home.
2. Home -> Motion -> Home.
3. Home -> System -> Home.
4. Home -> Wi-Fi -> Home.
5. Open eyes; check normal animation, 5 taps angry, shake dizzy, sleep and touch
   wake. Tilt through both landscape orientations and repeat the app sequence.
6. Check Improv provisioning/reconnection and touch placement in both orientations.

Pass: no retained pixels, frozen frames or misplaced touch; no `priv TX buffer`,
`spi transmit (queue) color failed` or `send color data failed` errors. Leave
Micro and eyes running for several minutes. Report visual behavior and logs.
If errors persist, capture internal DMA free/largest-block memory and boot logs
before broadening the change. Revert this branch commit to undo the fix.

## Exact sources

- BSP: https://github.com/waveshareteam/Waveshare-ESP32-components/tree/9f4030c6e5cb888ad4cc268bfa7584c93ad53e30/bsp/esp32_s3_touch_amoled_1_8
- LVGL port: https://github.com/espressif/esp-bsp/blob/f0ef9497efce684997ce391edd19733483e250a5/components/esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c
- SPI allocation: https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_driver_spi/src/gpspi/spi_master.c
- Queued LCD transfer: https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_lcd/spi/esp_lcd_panel_io_spi.c
- RGB callback registration: https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_lcd/rgb/esp_lcd_panel_rgb.c
