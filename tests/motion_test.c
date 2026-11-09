/* host/hos_motion.h with synthetic SDL readings: no SDL, controller or game. tests/test_motion.py builds and runs it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "hos_motion.h"

static int failures;
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (fabs(_a - _b) > (tol)) { \
    fprintf(stderr, "FAIL %s:%d: %s = %.5f, want %.5f\n", __FILE__, __LINE__, #a, _a, _b); failures++; } } while (0)

static void run(HosMotion *m, float ax, float ay, float az, float gx, float gy, float gz, float seconds, float sens_h,
                float sens_v, int inv_h, int inv_v) {
    const float a[3] = {ax, ay, az}, g[3] = {gx, gy, gz};
    const int n = (int)lroundf(seconds / 0.004f);
    for (int i = 0; i < n; i++) hos_motion_update(m, a, g, 0.004f, sens_h, sens_v, inv_h, inv_v);
}
#define FLAT 0, HOS_MOTION_G, 0           /* face up on a table: gravity's reaction on SDL's +Y */
#define UPRIGHT 0, 0, -HOS_MOTION_G       /* held upright, face to the player: the top (SDL's -Z) up */

static void neutral(const HosMotion *m) {   /* what the game folds into its recenter: always at rest */
    NEAR(m->out.accel[0], 0, 1e-6);
    NEAR(m->out.accel[1], 0, 1e-6);
    NEAR(m->out.accel[2], -1, 1e-6);
    NEAR(m->out.gyro[0], 0, 1e-6);
    NEAR(m->out.gyro[2], 0, 1e-6);
    NEAR(m->out.rotation[0] + m->out.rotation[1] + m->out.rotation[2], 0, 1e-6);
    NEAR(m->out.direction[0], 1, 1e-6);   /* (a tilt about x only) */
}

int main(void) {
    HosMotion m;

    /* before any reading: a pad lying still */
    hos_motion_reset(&m);
    neutral(&m);
    NEAR(m.out.direction[4] + m.out.direction[8], 2, 1e-6);

    /* lying still with a gyroscope that reads a small rate: learned and taken off, no aim moves */
    hos_motion_reset(&m);
    run(&m, FLAT, 0.012f, -0.008f, 0.010f, 3, 1, 1, 0, 0);
    NEAR(m.out.gyro[1], 0, 1e-4);
    NEAR(m.pitch, 0, 0.02);
    neutral(&m);

    /* turning left (about SDL's +Y, the pad flat) at one turn a second: the yaw slot reads 1 turn a second */
    hos_motion_reset(&m);
    run(&m, FLAT, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, FLAT, 0, HOS_MOTION_TAU, 0, 0.1f, 1, 1, 0, 0);
    NEAR(m.out.gyro[1], 1, 0.02);
    neutral(&m);

    /* tilting the front up (about SDL's +X) at 1 rad/s for half a second: the tilt matrix turns -0.5 rad, the rest
       neutral (the pitch rate itself never goes out) */
    hos_motion_reset(&m);
    run(&m, FLAT, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, FLAT, 1, 0, 0, 0.5f, 1, 1, 0, 0);
    NEAR(m.pitch, -0.5, 0.05);
    NEAR(m.out.direction[4], cosf(m.pitch), 1e-5);
    NEAR(m.out.direction[5], -sinf(m.pitch), 1e-5);
    NEAR(m.out.direction[7], sinf(m.pitch), 1e-5);
    NEAR(m.out.gyro[1], 0, 0.01);
    neutral(&m);

    /* held upright the same turn left is about the pad's -Z: the yaw slot still reads +1 (how the pad is held does
       not matter) */
    hos_motion_reset(&m);
    run(&m, UPRIGHT, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, UPRIGHT, 0, 0, -HOS_MOTION_TAU, 0.1f, 1, 1, 0, 0);
    NEAR(m.out.gyro[1], 1, 0.02);

    /* the sensitivities (each its own axis) and the inverts */
    hos_motion_reset(&m);
    run(&m, FLAT, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, FLAT, 0, HOS_MOTION_TAU, 0, 0.1f, 2, 0.5f, 1, 0);
    NEAR(m.out.gyro[1], -2, 0.04);
    NEAR(m.pitch, 0, 0.01);
    hos_motion_reset(&m);
    run(&m, FLAT, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    run(&m, FLAT, 1, 0, 0, 0.5f, 3, 1, 0, 1);
    NEAR(m.pitch, 0.5, 0.05);
    NEAR(m.out.gyro[1], 0, 0.01);

    /* picked up from flat to upright without the gyro seeing it: up settles on the measured gravity */
    hos_motion_reset(&m);
    run(&m, FLAT, 0, 0, 0, 0.5f, 1, 1, 0, 0);
    run(&m, UPRIGHT, 0, 0, 0, 5, 1, 1, 0, 0);
    NEAR(m.up[2], -1, 0.01);

    /* a stall (a second between readings) counts as one poll, not a jump */
    hos_motion_reset(&m);
    run(&m, FLAT, 0, 0, 0, 0.1f, 1, 1, 0, 0);
    {
        const float a[3] = {FLAT}, g[3] = {1, 0, 0};
        hos_motion_update(&m, a, g, 1.0f, 1, 1, 0, 0);
    }
    NEAR(m.pitch, -0.004, 1e-4);

    if (failures) return 1;
    puts("motion: ok");
    return 0;
}
