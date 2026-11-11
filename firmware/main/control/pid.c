/*
 * PID controller + time-proportioned output. See pid.h.
 */

#include "pid.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void pid_init(pid_ctrl_t *pid, float kp, float ti_s, float td_s, float setpoint)
{
    pid->kp         = kp;
    pid->ti_s       = ti_s;
    pid->td_s       = td_s;
    pid->d_filter_s = 60.0f;
    pid->setpoint   = setpoint;
    pid->out_min    = -1.0f;
    pid->out_max    =  1.0f;
    pid_reset(pid);
}

float pid_update(pid_ctrl_t *pid, float measured, float dt_s)
{
    if (dt_s <= 0.0f) dt_s = 1e-3f;

    /* Derivative on a low-pass filtered measurement (rate in °C/s). */
    float rate = 0.0f;
    if (!pid->have_meas) {
        pid->meas_filt = measured;
        pid->have_meas = true;
    } else {
        float a = dt_s / (pid->d_filter_s + dt_s);
        float prev = pid->meas_filt;
        pid->meas_filt += a * (measured - pid->meas_filt);
        rate = (pid->meas_filt - prev) / dt_s;
    }

    float error = pid->setpoint - measured;
    float pd = pid->kp * (error - pid->td_s * rate);

    /* Conditional integration: skip if the output is already saturated
       and this step would push it further the same way. */
    if (pid->ti_s > 0.0f) {
        float di = pid->kp * error * dt_s / pid->ti_s;
        float u  = pd + pid->integral;
        bool pushing_high = (u >= pid->out_max && di > 0.0f);
        bool pushing_low  = (u <= pid->out_min && di < 0.0f);
        if (!pushing_high && !pushing_low) {
            pid->integral = clampf(pid->integral + di, pid->out_min, pid->out_max);
        }
    }

    return clampf(pd + pid->integral, pid->out_min, pid->out_max);
}

void pid_set_setpoint(pid_ctrl_t *pid, float setpoint)
{
    pid->setpoint = setpoint;
}

void pid_reset(pid_ctrl_t *pid)
{
    pid->integral  = 0.0f;
    pid->meas_filt = 0.0f;
    pid->have_meas = false;
}


void tpo_init(tpo_t *t, float window_s, float min_pulse_s)
{
    t->window_s    = window_s;
    t->min_pulse_s = min_pulse_s;
    tpo_reset(t);
}

void tpo_reset(tpo_t *t)
{
    t->elapsed_s = 0.0f;
    t->on_for_s  = 0.0f;
    t->started   = false;
}

bool tpo_update(tpo_t *t, float duty, float dt_s)
{
    duty = clampf(duty, 0.0f, 1.0f);

    if (!t->started || t->elapsed_s >= t->window_s) {
        /* New window: plan the on-time. */
        float on = duty * t->window_s;
        if (on < t->min_pulse_s) {
            on = 0.0f;
        } else if (on > t->window_s - t->min_pulse_s) {
            on = t->window_s;        /* skip a too-short off gap */
        }
        t->started   = true;
        t->elapsed_s = 0.0f;
        t->on_for_s  = on;
    } else if (t->elapsed_s < t->on_for_s) {
        /* Mid-pulse: end it early if demand has dropped (but keep at
           least min_pulse so the plug isn't flicked). */
        float want = duty * t->window_s;
        if (want < t->min_pulse_s) want = t->min_pulse_s;
        if (t->elapsed_s >= want) {
            t->on_for_s = t->elapsed_s;
        }
    }

    bool on = t->elapsed_s < t->on_for_s;
    t->elapsed_s += dt_s;
    return on;
}
