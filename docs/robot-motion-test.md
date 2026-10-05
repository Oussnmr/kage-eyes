# Bench motion test

The firmware keeps the four DRV8833 inputs low at the first application
instruction. No motor or PCA9685 output is enabled at boot.

Reported wiring:

- Left N20: `OUT2` positive, `OUT1` negative; driver inputs `IN1=GPIO18`, `IN2=GPIO38`.
- Right N20: `OUT4` positive, `OUT3` negative; driver inputs `IN3=GPIO40`, `IN4=GPIO42`.
- PCA9685 on the dedicated I2C bus: `SDA=GPIO15`, `SCL=GPIO14`.
- Horizontal/pan servo: PCA channel `15`; vertical/tilt servo: PCA channel `14`.

Exact Kage bench commands:

| Command | Test pulse |
|---|---|
| `move H` | Horizontal servo, channel 15, 100 ms |
| `move V` | Vertical servo, channel 14, 100 ms |
| `move F` | Both N20 motors forward, 120 ms at 25% PWM |
| `move B` | Both N20 motors backward, 120 ms at 25% PWM |

Each command stops automatically. A second motion command is ignored while a
test pulse is active. Keep the wheels lifted and remove any load before the
first test. Use a common signal ground, keep the PCA9685 servo rail on the
regulated external supply, and never put the 5 V boost output on the PCA9685
logic `VCC` pin.
