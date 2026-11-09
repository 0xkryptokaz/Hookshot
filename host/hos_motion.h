/* (hookshot) A controller's motion sensors as the Switch's six-axis sensor reports them (host/pad_sdl.c fills it from
   SDL, host/nn_hid.c hands it to the game's GetSixAxisSensorStates). Header-only, no SDL or Windows, so
   tests/test_motion.py runs it as it is.

   SDL gives the accelerometer in m/s^2 and the gyroscope in rad/s, in its gamepad frame: +X right, +Y up out of the
   face buttons, +Z toward the player. The Switch wants G and revolutions a second, in its own frame: the same change of
   axes the yuzu family of emulators makes (x = -X, y = Z, z = -Y for the accelerometer; x = X, y = -Z, z = Y for the
   gyroscope), so a pad lying face up reads (0, 0, -1) G, the at-rest state nn_hid.c reported before.

   Beside the raw readings the state has the two derived fields games read: the rotation so far (revolutions, the
   gyroscope summed) and the direction (the pad's orientation as a 3x3 matrix, rows). The orientation is the gyroscope
   integrated as a quaternion with a gentle pull toward the accelerometer's gravity (Mahony's filter, proportional
   term only), so it does not drift in tilt. A gyroscope reads a small constant rate at rest (its bias): while the pad
   lies still that rate is learned and taken off, which is what keeps aiming from creeping.

   HOS_GYRO_FLIP=<axes> (any of x, y, z) turns those gyroscope axes the other way, for a controller whose driver reports
   one reversed. */
#pragma once
#include <math.h>
#include <string.h>

#define HOS_MOTION_G 9.80665f
#define HOS_MOTION_TAU 6.28318531f
#define HOS_MOTION_KP 0.5f           /* (the pull toward gravity, 1/s: slow enough that a turn is not undone) */
#define HOS_MOTION_STILL_RPS 0.03f   /* (under ~11 degrees a second the pad may be lying still) */
#define HOS_MOTION_STILL_S 0.8f      /* (still that long before the bias is learned) */

typedef struct {
    float accel[3];       /* G, the Switch's axes */
    float gyro[3];        /* revolutions a second, the Switch's axes, the learned bias taken off */
    float rotation[3];    /* revolutions, the gyroscope summed */
    float direction[9];   /* orientation, rows */
} HosMotionState;

typedef struct {
    float q[4];        /* w, x, y, z: the pad's frame into the world's */
    float bias[3];     /* rad/s, the Switch's axes */
    float still_s;     /* how long it has lain still */
    float flip[3];     /* 1 or -1 a gyroscope axis (HOS_GYRO_FLIP) */
    HosMotionState out;
} HosMotion;

static inline void hos_motion_reset(HosMotion *m, const char *flip) {
    memset(m, 0, sizeof *m);
    m->q[0] = 1;
    for (int i = 0; i < 3; i++) m->flip[i] = 1;
    for (const char *p = flip; p && *p; p++) {
        const int c = *p | 0x20;
        if (c >= 'x' && c <= 'z') m->flip[c - 'x'] = -1;
    }
    m->out.accel[2] = -1;
    m->out.direction[0] = m->out.direction[4] = m->out.direction[8] = 1;
}

/* One reading: SDL's accelerometer (m/s^2) and gyroscope (rad/s), dt seconds after the last one */
static inline void hos_motion_update(HosMotion *m, const float sdl_accel[3], const float sdl_gyro[3], float dt) {
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;   /* (a stall: no jump) */
    const float a[3] = {-sdl_accel[0] / HOS_MOTION_G, sdl_accel[2] / HOS_MOTION_G, -sdl_accel[1] / HOS_MOTION_G};
    float w[3] = {sdl_gyro[0] * m->flip[0], -sdl_gyro[2] * m->flip[1], sdl_gyro[1] * m->flip[2]};
    /* the bias: learned while the pad lies still (slow rate, about 1 G), taken off always */
    const float rate = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) / HOS_MOTION_TAU;
    const float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (rate < HOS_MOTION_STILL_RPS && fabsf(an - 1) < 0.1f) m->still_s += dt;   /* (pads read 1.03-1.06 G at rest) */
    else m->still_s = 0;
    if (m->still_s > HOS_MOTION_STILL_S) {
        const float k = dt / (dt + 0.5f);   /* (a half-second average) */
        for (int i = 0; i < 3; i++) m->bias[i] += (w[i] - m->bias[i]) * k;
    }
    for (int i = 0; i < 3; i++) w[i] -= m->bias[i];
    float *q = m->q;
    float wc[3] = {w[0], w[1], w[2]};
    if (an > 0.5f && an < 1.5f) {   /* (gravity's pull: not while the pad is being shaken) */
        /* where the filter expects the accelerometer's reading, R^T (0, 0, -1), and the error toward the reading */
        const float v[3] = {-2 * (q[1] * q[3] - q[0] * q[2]), -2 * (q[2] * q[3] + q[0] * q[1]),
                            -(1 - 2 * (q[1] * q[1] + q[2] * q[2]))};
        const float n[3] = {a[0] / an, a[1] / an, a[2] / an};
        const float e[3] = {n[1] * v[2] - n[2] * v[1], n[2] * v[0] - n[0] * v[2], n[0] * v[1] - n[1] * v[0]};
        for (int i = 0; i < 3; i++) wc[i] += HOS_MOTION_KP * e[i];
    }
    /* q += q (x) (0, w) dt / 2 */
    const float h = 0.5f * dt;
    const float dq[4] = {(-q[1] * wc[0] - q[2] * wc[1] - q[3] * wc[2]) * h, (q[0] * wc[0] + q[2] * wc[2] - q[3] * wc[1]) * h,
                         (q[0] * wc[1] - q[1] * wc[2] + q[3] * wc[0]) * h, (q[0] * wc[2] + q[1] * wc[1] - q[2] * wc[0]) * h};
    for (int i = 0; i < 4; i++) q[i] += dq[i];
    const float qn = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (qn > 0) for (int i = 0; i < 4; i++) q[i] /= qn;
    else q[0] = 1, q[1] = q[2] = q[3] = 0;
    HosMotionState *o = &m->out;
    for (int i = 0; i < 3; i++) {
        o->accel[i] = a[i];
        o->gyro[i] = w[i] / HOS_MOTION_TAU;
        o->rotation[i] += o->gyro[i] * dt;
    }
    const float w0 = q[0], x = q[1], y = q[2], z = q[3];
    const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w0 * z), 2 * (x * z + w0 * y),
                        2 * (x * y + w0 * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w0 * x),
                        2 * (x * z - w0 * y), 2 * (y * z + w0 * x), 1 - 2 * (x * x + y * y)};
    memcpy(o->direction, r, sizeof r);
}
