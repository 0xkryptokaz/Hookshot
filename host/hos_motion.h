/* (hookshot) A controller's motion sensors as OoT reads its six-axis state (host/pad_sdl.c fills it from SDL,
   host/nn_hid.c hands it to the game's GetSixAxisSensorStates). Header-only, no SDL or Windows, so
   tests/test_motion.py runs it as it is.

   OoT does not read the six-axis state as a plain sensor. What it aims with was found by spluton (issue 112, "Gyro
   aiming from SDL pads", tested on a DualSense) and corrected by Link-22 (the yaw rate's unit, tested on a GameSir T3
   Pro in Switch Pro mode, and the same feel as Eden on that pad):
   - the horizontal aim is the yaw rate, in the angular velocity's y slot, in turns a second (SDL's rad/s over 2 pi);
   - the vertical aim is the direction matrix, as a pure tilt about x (Rx(pitch)), the pitch rate integrated on its own;
   - the game folds the angles, the pitch rate and the acceleration into an aim recenter that cancels the tilt unless
     they are neutral: the angles and the pitch rate go out zero, the acceleration at rest (0, 0, -1).
   The rates are the pad's own, measured about the world's axes: an up vector from a complementary filter (the gyro's
   prediction pulled toward the measured gravity while it reads 0.65 to 1.35 G), the gyro projected onto right
   (pitch), up (yaw) and forward (roll). That makes the way the pad is held irrelevant: flat or upright, a turn toward
   the ceiling is up. The gyroscope's resting rate (its bias) is learned while the pad is still and taken off.

   Everything is in SDL's gamepad frame (+X right, +Y up out of the face buttons, +Z toward the player; m/s^2 and
   rad/s). The defaults are the two testers': the yaw rate as is (turning left aims left), the pitch rate negated. */
#pragma once
#include <math.h>
#include <string.h>

#define HOS_MOTION_G 9.80665f
#define HOS_MOTION_TAU 6.28318531f

typedef struct {
    float accel[3];       /* G (at rest, always: the game folds it into its recenter) */
    float gyro[3];        /* turns a second: the yaw rate in y, x and z zero */
    float rotation[3];    /* turns: zero (the game folds them into its recenter) */
    float direction[9];   /* rows: Rx(pitch), the vertical aim */
} HosMotionState;

typedef struct {
    float bias[3];   /* rad/s, SDL's axes */
    int bias_n;
    float up[3];     /* the world's up in the pad's frame (unit) */
    int up_ready;
    float pitch;     /* radians, the vertical aim's tilt */
    float rates[3];  /* (for tests and logs) pitch, yaw, roll about the world's axes, rad/s */
    HosMotionState out;
} HosMotion;

static inline void hos_motion_rest(HosMotionState *o, float pitch) {
    memset(o, 0, sizeof *o);
    o->accel[2] = -1;
    const float c = cosf(pitch), s = sinf(pitch);
    const float r[9] = {1, 0, 0, 0, c, -s, 0, s, c};
    memcpy(o->direction, r, sizeof r);
}

static inline void hos_motion_reset(HosMotion *m) {
    memset(m, 0, sizeof *m);
    m->up[1] = 1;
    hos_motion_rest(&m->out, 0);
}

/* One reading: SDL's accelerometer (m/s^2) and gyroscope (rad/s), dt seconds after the last one. sens multiplies
   both aims (1: as the pad turns); inv_h and inv_v turn the horizontal and the vertical the other way. */
static inline void hos_motion_update(HosMotion *m, const float accel[3], const float gyro[3], float dt, float sens,
                                     int inv_h, int inv_v) {
    if (!(dt > 0.001f) || dt > 0.05f) dt = 0.004f;   /* (a stall or a duplicate: one poll's worth) */
    /* the bias: learned while the pad is still (under 0.08 rad/s), fast at first, then slowly */
    const float b[3] = {gyro[0] - m->bias[0], gyro[1] - m->bias[1], gyro[2] - m->bias[2]};
    if (b[0] * b[0] + b[1] * b[1] + b[2] * b[2] < 0.0064f) {
        const float k = m->bias_n < 256 ? 0.06f : 0.004f;
        for (int i = 0; i < 3; i++) m->bias[i] += b[i] * k;
        m->bias_n++;
    }
    const float w[3] = {gyro[0] - m->bias[0], gyro[1] - m->bias[1], gyro[2] - m->bias[2]};
    /* up: the gyro's prediction (a world vector seen from a frame turning at w turns the other way), pulled toward
       the measured gravity while it is about 1 G */
    const float alen = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    const int valid = alen > 0.65f * HOS_MOTION_G && alen < 1.35f * HOS_MOTION_G;
    float *up = m->up;
    if (!m->up_ready) {
        if (valid) {
            for (int i = 0; i < 3; i++) up[i] = accel[i] / alen;
            m->up_ready = 1;
        }
    } else {
        float p[3] = {up[0] + (-w[1] * up[2] + w[2] * up[1]) * dt, up[1] + (-w[2] * up[0] + w[0] * up[2]) * dt,
                      up[2] + (-w[0] * up[1] + w[1] * up[0]) * dt};
        float n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (n > 0.001f) {
            for (int i = 0; i < 3; i++) p[i] /= n;
            if (valid) {
                const float k = dt / (0.5f + dt);   /* (a half-second pull) */
                for (int i = 0; i < 3; i++) p[i] += (accel[i] / alen - p[i]) * k;
                n = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                if (n > 0.001f) for (int i = 0; i < 3; i++) p[i] /= n;
            }
            memcpy(up, p, sizeof p);
        }
    }
    /* right: the pad's own x, level (gravity taken out); when the pad points x up, its z instead */
    float right[3] = {1 - up[0] * up[0], -up[0] * up[1], -up[0] * up[2]};
    float rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (rl < 0.1f) {
        right[0] = -up[0] * up[2], right[1] = -up[1] * up[2], right[2] = 1 - up[2] * up[2];
        rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    }
    for (int i = 0; i < 3; i++) right[i] /= rl;
    const float fwd[3] = {right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2],
                          right[0] * up[1] - right[1] * up[0]};
    m->rates[0] = w[0] * right[0] + w[1] * right[1] + w[2] * right[2];
    m->rates[1] = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
    m->rates[2] = w[0] * fwd[0] + w[1] * fwd[1] + w[2] * fwd[2];
    const float h = m->rates[1] * sens * (inv_h ? -1.0f : 1.0f);
    const float v = -m->rates[0] * sens * (inv_v ? -1.0f : 1.0f);
    m->pitch += v * dt;
    if (m->pitch > 3.14159265f) m->pitch -= HOS_MOTION_TAU;   /* (an angle: kept in one turn) */
    if (m->pitch < -3.14159265f) m->pitch += HOS_MOTION_TAU;
    hos_motion_rest(&m->out, m->pitch);
    m->out.gyro[1] = h / HOS_MOTION_TAU;
}
