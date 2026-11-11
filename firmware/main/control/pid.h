/*
 * PID temperature controller
 *
 * Output is in [-1.0, 1.0]: positive = heat duty, negative = cool duty.
 * Gains are in physical units so they don't depend on the loop period:
 *
 *   u = Kp * ( e + (1/Ti) * ∫e dt  -  Td * dT/dt )
 *
 *   Kp  [1/°C]  — duty per °C of error
 *   Ti  [s]     — integral time (0 disables the integral)
 *   Td  [s]     — derivative time
 *
 * Details that keep a slow, laggy thermal plant from overshooting:
 *  - Derivative acts on the (low-pass filtered) measurement, not the error,
 *    so setpoint changes don't kick it and 0.0625 °C sensor steps don't
 *    turn into spikes.
 *  - Conditional integration: the integrator is frozen whenever the output
 *    is saturated in the direction it would push (e.g. full heat during
 *    warm-up), so it can't wind up and carry the temperature past target.
 *  - The integrator holds the steady-state duty; it is kept across
 *    setpoint changes (bumpless) and only cleared by pid_reset().
 */
#pragma once

#include <stdbool.h>

typedef struct {
    float kp, ti_s, td_s;
    float d_filter_s;        /* time constant of the measurement filter for D */
    float setpoint;
    float integral;          /* in output units (duty), clamped to [out_min, out_max] */
    float meas_filt;
    bool  have_meas;
    float out_min, out_max;
} pid_ctrl_t;

void pid_init(pid_ctrl_t *pid, float kp, float ti_s, float td_s, float setpoint);

/**
 * Compute one PID step.
 * @param measured  Current process variable (°C)
 * @param dt_s      Seconds since the previous call
 * @return          Control output in [-1.0, 1.0]
 */
float pid_update(pid_ctrl_t *pid, float measured, float dt_s);

void pid_set_setpoint(pid_ctrl_t *pid, float setpoint);

/** Clear integrator and derivative state (e.g. after a sensor fault). */
void pid_reset(pid_ctrl_t *pid);


/*
 * Time-proportioned output
 *
 * Turns a duty in [0, 1] into on/off switching for a relay or smart plug:
 * at the start of each window the output turns on for duty * window.
 * Pulses shorter than min_pulse are skipped (or stretched to the whole
 * window), which bounds how often the plug relay clicks. If the demand
 * drops mid-pulse the pulse is ended early; it is never restarted within
 * the same window.
 */
typedef struct {
    float window_s;
    float min_pulse_s;
    float elapsed_s;         /* time since the current window started */
    float on_for_s;          /* planned on-time in the current window */
    bool  started;
} tpo_t;

void tpo_init(tpo_t *t, float window_s, float min_pulse_s);

/** Advance by dt_s with the current duty; returns true if the output should be on. */
bool tpo_update(tpo_t *t, float duty, float dt_s);

/** Force off and start a fresh window on the next update. */
void tpo_reset(tpo_t *t);
