/* host/hos_motion.h with synthetic SDL readings: no SDL, controller or game. tests/test_motion.py builds and runs it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "hos_motion.h"

static int failures;
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (fabs(_a - _b) > (tol)) { \
    fprintf(stderr, "FAIL %s:%d: %s = %.5f, want %.5f\n", __FILE__, __LINE__, #a, _a, _b); failures++; } } while (0)

static void run(HosMotion *m, float ax, float ay, float az, float gx, float gy, float gz, float seconds) {
    const float a[3] = {ax, ay, az}, g[3] = {gx, gy, gz};
    for (int i = 0; i < (int)(seconds / 0.004f + 0.5f); i++) hos_motion_update(m, a, g, 0.004f);
}

int main(void) {
    HosMotion m;

    /* at rest, before any reading: the state nn_hid.c reported before */
    hos_motion_reset(&m, NULL);
    NEAR(m.out.accel[2], -1, 1e-6);
    NEAR(m.out.direction[0] + m.out.direction[4] + m.out.direction[8], 3, 1e-6);

    /* face up on a table (SDL: gravity's reaction on +Y): (0, 0, -1) G, no turn, the orientation stays put */
    hos_motion_reset(&m, NULL);
    run(&m, 0, HOS_MOTION_G, 0, 0, 0, 0, 2);
    NEAR(m.out.accel[0], 0, 1e-5);
    NEAR(m.out.accel[1], 0, 1e-5);
    NEAR(m.out.accel[2], -1, 1e-5);
    NEAR(m.out.direction[0] + m.out.direction[4] + m.out.direction[8], 3, 1e-3);

    /* a turn about SDL's +Y (yaw) at one revolution a second is the Switch's +z at 1 rev/s; a quarter second of it a
       quarter turn. (Held in the air: the accelerometer 1.3 G, so the bias is not learned) */
    hos_motion_reset(&m, NULL);
    run(&m, 0, 1.3f * HOS_MOTION_G, 0, 0, HOS_MOTION_TAU, 0, 0.25f);
    NEAR(m.out.gyro[2], 1, 1e-4);
    NEAR(m.out.gyro[0], 0, 1e-6);
    NEAR(m.out.gyro[1], 0, 1e-6);
    NEAR(m.out.rotation[2], 0.25, 5e-3);   /* (to a reading) */
    /* SDL's X is the Switch's x, SDL's Z the Switch's -y */
    hos_motion_reset(&m, NULL);
    run(&m, 0, 1.3f * HOS_MOTION_G, 0, 1, 0, 2, 0.1f);
    NEAR(m.out.gyro[0], 1 / HOS_MOTION_TAU, 1e-5);
    NEAR(m.out.gyro[1], -2 / HOS_MOTION_TAU, 1e-5);

    /* HOS_GYRO_FLIP=y turns the Switch's y the other way */
    hos_motion_reset(&m, "y");
    run(&m, 0, 1.3f * HOS_MOTION_G, 0, 0, 0, 2, 0.1f);
    NEAR(m.out.gyro[1], 2 / HOS_MOTION_TAU, 1e-5);

    /* a gyroscope that reads a small rate lying still: learned and taken off, so aiming does not creep */
    hos_motion_reset(&m, NULL);
    run(&m, 0, HOS_MOTION_G, 0, 0.02f, -0.01f, 0.015f, 6);
    NEAR(m.out.gyro[0], 0, 2e-4);
    NEAR(m.out.gyro[1], 0, 2e-4);
    NEAR(m.out.gyro[2], 0, 2e-4);
    /* and a real turn afterwards still reads in full */
    run(&m, 0, 1.3f * HOS_MOTION_G, 0, 0, HOS_MOTION_TAU, 0, 0.1f);
    NEAR(m.out.gyro[2], 1, 2e-3);

    /* tilted and lying still, the orientation settles on what the accelerometer says (the filter pulls the right way):
       the gravity the orientation predicts, R^T (0, 0, -1), meets the reading. Pad on its side: SDL +X up */
    hos_motion_reset(&m, NULL);
    run(&m, HOS_MOTION_G, 0, 0, 0, 0, 0, 30);
    {
        const float *r = m.out.direction;
        NEAR(-r[6], m.out.accel[0], 0.02);
        NEAR(-r[7], m.out.accel[1], 0.02);
        NEAR(-r[8], m.out.accel[2], 0.02);
        NEAR(r[0] * r[0] + r[1] * r[1] + r[2] * r[2], 1, 1e-3);   /* (still a rotation) */
    }

    /* a stall (a second between readings) counts as at most 0.1 s */
    hos_motion_reset(&m, NULL);
    {
        const float a[3] = {0, 1.3f * HOS_MOTION_G, 0}, g[3] = {0, HOS_MOTION_TAU, 0};
        hos_motion_update(&m, a, g, 1.0f);
    }
    NEAR(m.out.rotation[2], 0.1, 1e-4);

    if (failures) return 1;
    puts("motion: ok");
    return 0;
}
