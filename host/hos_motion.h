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
   (pitch), up (yaw) and forward (roll). That makes the way the pad is held irrelevant for the turn: flat or upright, a turn
   toward the ceiling is up. The vertical aim itself is the pad's tilt from flat, as a Switch reports it. The gyroscope's resting rate (its bias) is learned while the pad is still (both sensors steady, not
   merely slow) and taken off, and the vertical aim is pulled gently toward the tilt gravity measures, so it cannot drift.

   Everything is in SDL's gamepad frame (+X right, +Y up out of the face buttons, +Z toward the player; m/s^2 and
   rad/s). The defaults are the two testers': the yaw rate as is (turning left aims left), the pitch rate negated. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define HOS_MOTION_G 9.80665f
#define HOS_MOTION_TAU 6.28318531f

typedef struct {
    float accel[3];       /* G (at rest, always: the game folds it into its recenter) */
    float gyro[3];        /* turns a second: the yaw rate in y, x and z zero */
    float rotation[3];    /* turns: zero (the game folds them into its recenter) */
    float direction[9];   /* rows: Rx(pitch), the vertical aim */
} HosMotionState;

/* one reading's state as GetSixAxisSensorStates hands it out: n counts the readings, dt_ns is the time since the one
   before (host/pad_sdl.c keeps the last HOS_MOTION_HISTORY) */
typedef struct {
    HosMotionState st;
    int64_t n, dt_ns;
} HosMotionSample;
#define HOS_MOTION_HISTORY 32

typedef struct {
    float bias[3];   /* rad/s, SDL's axes */
    float still_t;   /* seconds the pad has been still (bias learning starts after HOS_MOTION_STILL_S) */
    float learned_t; /* seconds of stillness learned from so far (fast at first, then slowly) */
    float gmean[3], gvar;   /* the gyro's short-window mean (rad/s) and variance (summed over its axes) */
    float amean, avar;      /* the accelerometer's magnitude's mean, and its vector's variance summed over the axes (G):
                               a pad turning at a steady rate keeps its magnitude but not its direction */
    float avec[3];          /* (the accelerometer's vector's mean, G) */
    int stats_ready;
    float last_accel[3], last_gyro[3];   /* (the last reading: a repeat of it is no new reading) */
    float pending_dt;                    /* (time from repeats, carried into the next new reading) */
    float up[3];     /* the world's up in the pad's frame (unit) */
    int up_ready;
    float pitch;     /* radians, the vertical aim's tilt */
    float tilt_last; /* radians, the pad's tilt about its x axis at the last level reading */
    float tilt_acc;  /* radians, that tilt from flat, unwrapped (so it scales by any sensitivity without a jump at a
                        half turn) */
    float tilt_sv;   /* the vertical scale the aim is at (0 before the first reading): a new one rescales it */
    float tilt_ref;  /* radians, the pad's playing hold (its tilt from flat): the sensitivity and the invert scale the
                        tilt's change from it, and the aim there is the tilt itself. Taken when the filter starts, and
                        again (eased in) once the pad is picked up off a table and held */
    float ref_to;    /* (the hold tilt_ref eases to; ref_easing while it does) */
    int ref_easing, was_set_down;
    float held_t;    /* seconds held steady in the hand since it was last set down */
    int tilt_ready;
    float rates[3];  /* (for tests and logs) pitch, yaw, roll about the world's axes, rad/s */
    HosMotionState out;
} HosMotion;

/* stillness: the gyro's spread under HOS_MOTION_STILL_GYRO rad/s and the accelerometer's vector's under
   HOS_MOTION_STILL_ACCEL G, about 1 G, for HOS_MOTION_STILL_S. Both are summed over the three axes: 0.015 rad/s is
   about 0.009 on each and 0.0173 G about 0.01 G on each, room for noisier third-party sensors. What keeps aiming from
   being learned as bias is the hand: its tremor alone is several times the gyro's threshold. The accelerometer's
   vector adds a steady pitch or roll faster than about 5 degrees a second (its direction moves past the window's lag);
   a steady turn about gravity leaves it unchanged, so only a pad on a turntable, untouched, could be mistaken. An
   offset of any size is learned on a table. */
#define HOS_MOTION_STILL_GYRO 0.015f
#define HOS_MOTION_STILL_ACCEL 0.0173f   /* (0.01 G on each axis, summed over three like the gyro's) */
#define HOS_MOTION_STILL_S 0.5f
#define HOS_MOTION_WINDOW_S 0.2f   /* (the short window's time constant) */
#define HOS_MOTION_DT_MAX 0.1f     /* (a longer gap, a hitch, counts as this long) */
#define HOS_MOTION_TILT_S 1.0f     /* (the vertical aim's pull toward the measured tilt) */
#define HOS_MOTION_HELD_GYRO 0.05f   /* (held steady in a hand: the gyro's spread under this, rad/s, summed) */
#define HOS_MOTION_HELD_S 0.5f       /* (... for this long, after being set down: the new playing hold) */
#define HOS_MOTION_REF_S 0.3f        /* (the new hold eased in over about this) */
#define HOS_MOTION_TILT_ROLL 0.25f /* (... only while |up[0]| is under this: about 15 degrees of roll) */

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

static inline float hos_motion_wrap(float a) {
    while (a > 3.14159265f) a -= HOS_MOTION_TAU;
    while (a < -3.14159265f) a += HOS_MOTION_TAU;
    return a;
}

/* One reading: SDL's accelerometer (m/s^2) and gyroscope (rad/s), dt seconds after the last one. sens_h and sens_v
   scale the horizontal and the vertical aim (1: as the pad turns); inv_h and inv_v turn them the other way. Returns 0
   for a repeat of the last reading (no new reading: its time goes into the next), 1 otherwise. */
static inline int hos_motion_update(HosMotion *m, const float accel[3], const float gyro[3], float dt, float sens_h,
                                    float sens_v, int inv_h, int inv_v) {
    if (!(dt >= 0)) dt = 0;
    if (m->stats_ready && !memcmp(accel, m->last_accel, sizeof m->last_accel) &&
        !memcmp(gyro, m->last_gyro, sizeof m->last_gyro)) {
        m->pending_dt += dt;
        return 0;
    }
    memcpy(m->last_accel, accel, sizeof m->last_accel), memcpy(m->last_gyro, gyro, sizeof m->last_gyro);
    dt += m->pending_dt, m->pending_dt = 0;
    if (dt > HOS_MOTION_DT_MAX) dt = HOS_MOTION_DT_MAX;   /* (a hitch: its time, up to a limit) */
    const float alen = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    const float ag = alen / HOS_MOTION_G;
    /* the bias: learned from the short window's mean while the pad is still (both sensors steady, about 1 G) */
    if (!m->stats_ready) {
        memcpy(m->gmean, gyro, sizeof m->gmean), m->gvar = 0.01f, m->amean = ag, m->avar = 0.01f;   /* (not still until
                                                                                        the window has seen it) */
        for (int i = 0; i < 3; i++) m->avec[i] = accel[i] / HOS_MOTION_G;
        m->stats_ready = 1;
    } else if (dt > 0) {
        const float k = dt / (HOS_MOTION_WINDOW_S + dt);
        float gv = 0;
        for (int i = 0; i < 3; i++) {
            const float d = gyro[i] - m->gmean[i];
            m->gmean[i] += d * k;
            gv += d * d;
        }
        m->gvar += (gv - m->gvar) * k;
        m->amean += (ag - m->amean) * k;
        float av = 0;
        for (int i = 0; i < 3; i++) {
            const float d = accel[i] / HOS_MOTION_G - m->avec[i];
            m->avec[i] += d * k;
            av += d * d;
        }
        m->avar += (av - m->avar) * k;
    }
    const int still = m->gvar < HOS_MOTION_STILL_GYRO * HOS_MOTION_STILL_GYRO &&
                      m->avar < HOS_MOTION_STILL_ACCEL * HOS_MOTION_STILL_ACCEL && m->amean > 0.9f && m->amean < 1.1f;
    m->still_t = still ? m->still_t + dt : 0;
    if (m->still_t >= HOS_MOTION_STILL_S && dt > 0) {
        const float k = dt / ((m->learned_t < 2 ? 0.25f : 3.0f) + dt);   /* (fast for the first 2 s still, then slowly) */
        for (int i = 0; i < 3; i++) m->bias[i] += (m->gmean[i] - m->bias[i]) * k;
        m->learned_t += dt;
    }
    const float w[3] = {gyro[0] - m->bias[0], gyro[1] - m->bias[1], gyro[2] - m->bias[2]};
    /* up: the gyro's prediction (a world vector seen from a frame turning at w turns the other way), pulled toward
       the measured gravity while it is about 1 G */
    const int valid = alen > 0.65f * HOS_MOTION_G && alen < 1.35f * HOS_MOTION_G;
    float *up = m->up;
    int started = 0;
    if (!m->up_ready) {
        if (valid) {
            for (int i = 0; i < 3; i++) up[i] = accel[i] / alen;
            m->up_ready = 1, started = 1;
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
    const float sh = sens_h * (inv_h ? -1.0f : 1.0f), sv = -sens_v * (inv_v ? -1.0f : 1.0f);
    /* the vertical aim is the pad's real tilt, as a Switch reports it: measured from the pad lying flat (its face up),
       however it is held when the filter starts (at launch, when Motion controls is turned on, on a pad switch or
       reconnect); the game recenters from it each time it aims. It starts at the pad's tilt about its x from flat (held
       rolled, the nose's angle above level instead: roll does not change that), and the tilt pull settles it once the
       pad is level. Before, it started at 0 in whatever pose the pad was in, and a pad held tilted then aimed off by
       that tilt for good (Link-22). The sensitivity and the invert scale only the tilt's change from where the filter
       started (tilt_ref), so at rest in that hold the aim is the real tilt at any setting; scaling the whole angle from
       flat moved a normal hold of 0.7 rad to vertical at 200% and mirrored it with the invert (hckrman101). A new
       setting rescales the change. With the default (sv = -1) it is the real tilt everywhere. */
    if (started) {
        const float e = -up[2] > 1 ? 1 : -up[2] < -1 ? -1 : -up[2];
        m->tilt_ref = fabsf(up[0]) < HOS_MOTION_TILT_ROLL ? atan2f(-up[2], up[1]) : asinf(e);
        m->pitch = hos_motion_wrap(-m->tilt_ref);
        m->tilt_sv = sv;
    } else if (sv != m->tilt_sv && m->tilt_sv != 0) {
        m->pitch = hos_motion_wrap(m->tilt_ready ? -m->tilt_ref + (m->tilt_acc - m->tilt_ref) * sv
                                                 : -m->tilt_ref + (m->pitch + m->tilt_ref) / m->tilt_sv * sv);
        m->tilt_sv = sv;
    }
    m->pitch = hos_motion_wrap(m->pitch + m->rates[0] * sv * dt);
    /* the vertical aim's drift: pulled gently toward the tilt gravity measures (the pad's tilt about its x axis from
       flat, scaled the same way), so leftover bias or timing error cannot add up. Only while the pad's
       x is about level (rolled under about 15 degrees): rolled, the tilt about the pad's own x moves faster than the
       pitch about the levelled x (by 1 / sqrt(1 - up[0]^2)), and the pull would drag the aim past each move
       (hckrman101); what builds up while rolled is corrected once it is level again. Not while shaken either. */
    if (!started && m->up_ready && valid && fabsf(up[0]) < HOS_MOTION_TILT_ROLL) {
        const float tilt = atan2f(-up[2], up[1]);   /* (it grows at the x rate: d/dt = w[0]) */
        /* (anchored so the aim stays where it is: the first time the pad is level may be after it moved while rolled,
           or held tilted when the filter started; sv is never 0, the menu's least sensitivity is 10%) */
        if (!m->tilt_ready) {   /* (the tilt from flat itself, on tilt_ref's branch: near face-down the two could
                                   be a turn apart, and at a sensitivity that is no whole number that is half a turn) */
            m->tilt_acc = m->tilt_ref + hos_motion_wrap(tilt - m->tilt_ref), m->tilt_ready = 1;
        } else {
            m->tilt_acc += hos_motion_wrap(tilt - m->tilt_last);
        }
        m->tilt_last = tilt;
        const float target = hos_motion_wrap(-m->tilt_ref + (m->tilt_acc - m->tilt_ref) * sv);
        m->pitch = hos_motion_wrap(m->pitch + hos_motion_wrap(target - m->pitch) * (dt / (HOS_MOTION_TILT_S + dt)));
    }
    /* the playing hold, again: a pad that was set down (still on a table) and is then picked up and held steady for a
       moment has its new hold eased in as tilt_ref, so a start on the desk does not leave the sensitivity and the
       invert measured from flat (hckrman101). The aim moves with tilt_ref by -(1 + sv) per radian: nothing at the
       defaults (sv = -1), else an ease to the real tilt at rest in the new hold. */
    if (m->still_t >= HOS_MOTION_STILL_S) m->was_set_down = 1, m->held_t = 0;
    else {
        /* (held: the gyro's spread a hand's, and the pad not turning, so a smooth sweep is not a hold) */
        float r = 0;
        for (int i = 0; i < 3; i++) r += (m->gmean[i] - m->bias[i]) * (m->gmean[i] - m->bias[i]);
        const int held = m->gvar < HOS_MOTION_HELD_GYRO * HOS_MOTION_HELD_GYRO && r < HOS_MOTION_HELD_GYRO * HOS_MOTION_HELD_GYRO;
        if (m->was_set_down && m->tilt_ready && held && fabsf(up[0]) < HOS_MOTION_TILT_ROLL) m->held_t += dt;
        else if (!held) m->held_t = 0;
    }
    if (m->was_set_down && m->held_t >= HOS_MOTION_HELD_S) {
        m->ref_to = m->tilt_acc, m->ref_easing = 1, m->was_set_down = 0, m->held_t = 0;
    }
    if (m->ref_easing && dt > 0) {
        const float d = (m->ref_to - m->tilt_ref) * (dt / (HOS_MOTION_REF_S + dt));
        m->tilt_ref += d;
        m->pitch = hos_motion_wrap(m->pitch - (1 + sv) * d);
        if (fabsf(m->ref_to - m->tilt_ref) < 1e-4f) m->ref_easing = 0;
    }
    hos_motion_rest(&m->out, m->pitch);
    m->out.gyro[1] = m->rates[1] * sh / HOS_MOTION_TAU;
    return 1;
}
