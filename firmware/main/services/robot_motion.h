#pragma once

#include "robot_behavior.h"

enum RobotMove {
    ROBOT_MOVE_HORIZONTAL,
    ROBOT_MOVE_VERTICAL,
    ROBOT_MOVE_FORWARD,
    ROBOT_MOVE_BACKWARD,
};

enum RobotDrive {
    ROBOT_DRIVE_FORWARD,
    ROBOT_DRIVE_BACKWARD,
    ROBOT_DRIVE_LEFT,
    ROBOT_DRIVE_RIGHT,
};

enum RobotServoNudge {
    ROBOT_PAN_LEFT,
    ROBOT_PAN_RIGHT,
    ROBOT_TILT_UP,
    ROBOT_TILT_DOWN,
};

// Called as the very first app_main operation.  It does not touch I2C or the
// servos; it only guarantees that every DRV8833 input starts at logic zero.
void robot_motion_safe_boot(void);
// Called after NVS has been initialized. Loads saved servo zeros and starts
// the smooth servo worker.
void robot_motion_begin(void);

// Starts one short, self-stopping bench-test pulse.  A command received while
// another pulse is active is ignored rather than extending the movement.
void robot_motion_command(RobotMove move);

// Hold-to-run controls used by the phone UI. Each drive request refreshes a
// short watchdog; loss of Wi-Fi or a released button therefore stops motion.
void robot_drive_hold(RobotDrive drive);
// Differential-drive input from the phone controller. Values are -100..100;
// positive is forward for the matching tracked side.
void robot_drive_analog(int left_percent, int right_percent);
void robot_motion_stop(void);
void robot_servo_nudge(RobotServoNudge nudge);
// Absolute virtual positions from the phone controller (0..100).  The
// firmware owns the smooth trajectory and cable-safe limits.
void robot_servo_targets(int pan_percent, int tilt_percent);
void robot_servo_pose(int pan_state, int tilt_state);
void robot_servo_adjust(int pan_delta_us, int tilt_delta_us);
void robot_servo_calibration_begin(void);
bool robot_servo_calibration_save(void);
// Runtime limits selected from the control page. Values are percentages and
// are persisted in NVS, so changing them later does not require another flash.
bool robot_motion_settings(int motor_limit, int pan_range, int tilt_range,
                           int servo_speed);
void robot_motion_behavior(int behavior, int duration_ms);
void robot_motion_cancel_behavior(void);
void robot_motion_start_dance(void);
void robot_motion_stop_dance(void);
void robot_motion_start_360(void);
