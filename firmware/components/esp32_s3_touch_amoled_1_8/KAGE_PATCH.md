# Local QSPI display fix

Vendored from waveshare/esp32_s3_touch_amoled_1_8 **2.0.3**, upstream commit
`9f4030c6e5cb888ad4cc268bfa7584c93ad53e30`, directory
`bsp/esp32_s3_touch_amoled_1_8`. Apache-2.0 license retained in LICENSE.

Archive: https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8/versions/2.0.3

Only `esp32_s3_touch_amoled_1_8.c` differs from the published component:

- Register CO5300 QSPI with `lvgl_port_add_disp`, not `lvgl_port_add_disp_rgb`.
  The SPI registration installs `on_color_trans_done`; LVGL must not reuse the
  software rotation buffer before that callback. RGB registration also passes
  a CO5300 panel to an incompatible RGB panel callback registration function.
- Set `buff_dma = true`: esp_lvgl_port 2.9.0 uses the same allocation capabilities
  for both the draw buffer and the software rotation buffer.
- Remove the now-unused RGB configuration.

The application's sdkconfig.defaults sets buffer height to 32, so each buffer is
368 * 32 * 2 = 23,552 bytes. Single buffering, RGB565 byte swap, software rotation,
CO5300 coordinate rounding, panel offsets and all non-display BSP code are kept.
No full-frame redraw or RGB anti-tearing mode is enabled.

To remove the workaround after an upstream fix is verified, remove the local
component and the application's override_path together, then select the verified
upstream version. Do not silently replace this directory with a newer BSP.
