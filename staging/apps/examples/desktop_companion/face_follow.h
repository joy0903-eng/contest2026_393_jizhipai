/****************************************************************************
 * apps/examples/desktop_companion/face_follow.h
 *
 * Face-follow module for the AI-VOX3.
 *
 * The board has NO camera, so this is the DEGRADED servo-demo implementation
 * (plan §7.1): when a "speaking" event occurs, the head pan/tilt servos do a
 * gentle swing-and-return animation to simulate "looking at the user".
 *
 * The real-detection interface (face_follow_update(x,y)) is kept so a future
 * OV2640 + ESP-DL path can drive the same servos without app changes.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_FACE_FOLLOW_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_FACE_FOLLOW_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Enable/disable face-follow (demo) mode. */
int face_follow_enable(bool enable);

/* Feed detection coordinates (x,y in -100..100). In demo (no-camera) mode
 * this is a no-op placeholder for the future real detector. */
int face_follow_update(int x, int y);

/* Advance the demo animation by one tick. Call periodically from the loop.
 * Drives the head servos while an animation is active. */
int face_follow_demo_tick(void);

/* Trigger the "look at user" head animation (called on a speak event). */
int face_follow_trigger_speak(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_FACE_FOLLOW_H */
