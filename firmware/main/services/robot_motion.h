#pragma once

enum RobotMove {
    ROBOT_MOVE_HORIZONTAL,
    ROBOT_MOVE_VERTICAL,
    ROBOT_MOVE_FORWARD,
    ROBOT_MOVE_BACKWARD,
};

// Called as the very first app_main operation.  It does not touch I2C or the
// servos; it only guarantees that every DRV8833 input starts at logic zero.
void robot_motion_safe_boot(void);

// Starts one short, self-stopping bench-test pulse.  A command received while
// another pulse is active is ignored rather than extending the movement.
void robot_motion_command(RobotMove move);
