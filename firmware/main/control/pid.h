/*
 * Simple PID controller with anti-windup
 * Output clamped to [-1.0, 1.0]
 *   negative = cool, positive = heat
 */
#pragma once

typedef struct {
    float kp, ki, kd;
    float setpoint;
    float integral;
    float prev_error;
    float out_min, out_max;
    float integral_limit;
} pid_ctrl_t;

/**
 * Initialise the PID controller.
 */
void pid_init(pid_ctrl_t *pid, float kp, float ki, float kd, float setpoint);

/**
 * Compute one PID step.
 * @param measured  Current process variable (temperature in C)
 * @return          Control output in [-1.0, 1.0]
 */
float pid_update(pid_ctrl_t *pid, float measured);

/**
 * Change the setpoint at runtime.
 */
void pid_set_setpoint(pid_ctrl_t *pid, float setpoint);

/**
 * Reset integrator and derivative state.
 */
void pid_reset(pid_ctrl_t *pid);
