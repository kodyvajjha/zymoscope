/*
 * Simple PID controller with anti-windup and output clamping.
 */

#include "pid.h"

void pid_init(pid_ctrl_t *pid, float kp, float ki, float kd, float setpoint)
{
    pid->kp       = kp;
    pid->ki       = ki;
    pid->kd       = kd;
    pid->setpoint = setpoint;

    pid->integral   = 0.0f;
    pid->prev_error = 0.0f;

    pid->out_min = -1.0f;
    pid->out_max =  1.0f;

    /* Anti-windup: limit the integrator so that ki*integral alone can
       never exceed the output range. */
    pid->integral_limit = (ki > 0.001f) ? (1.0f / ki) : 1000.0f;
}

float pid_update(pid_ctrl_t *pid, float measured)
{
    float error = pid->setpoint - measured;

    /* Proportional */
    float p_term = pid->kp * error;

    /* Integral with anti-windup clamp */
    pid->integral += error;
    if (pid->integral >  pid->integral_limit) pid->integral =  pid->integral_limit;
    if (pid->integral < -pid->integral_limit) pid->integral = -pid->integral_limit;
    float i_term = pid->ki * pid->integral;

    /* Derivative (on error) */
    float d_term = pid->kd * (error - pid->prev_error);
    pid->prev_error = error;

    /* Sum and clamp output */
    float output = p_term + i_term + d_term;
    if (output > pid->out_max) output = pid->out_max;
    if (output < pid->out_min) output = pid->out_min;

    return output;
}

void pid_set_setpoint(pid_ctrl_t *pid, float setpoint)
{
    pid->setpoint = setpoint;
}

void pid_reset(pid_ctrl_t *pid)
{
    pid->integral   = 0.0f;
    pid->prev_error = 0.0f;
}
