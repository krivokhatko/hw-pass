#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run the screen and the buttons. Does not return. */
void ui_run(void);

/* Requests from other tasks. ui_run does the work. */
void ui_flip(void);     /* Turn the screen 180 degrees and keep the setting, until the device is turned. */
void ui_dfu(void);      /* Show a message, then restart into download mode. */
void ui_web(bool on);   /* Start (true) or stop (false) the Wi-Fi setup mode. */
void ui_pair(void);     /* Pair a computer over Bluetooth: the screen shows the steps. */

/* Turn the sounds on or off and keep the setting. Works at once, from
 * any task. */
void ui_sound(bool on);

#ifdef __cplusplus
}
#endif
