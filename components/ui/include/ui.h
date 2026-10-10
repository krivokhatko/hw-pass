#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Run the screen and the buttons. Does not return. */
void ui_run(void);

/* Requests from other tasks. ui_run does the work. */
void ui_flip(void);     /* Turn the screen 180 degrees and keep the setting. */
void ui_dfu(void);      /* Show a message, then restart into download mode. */

#ifdef __cplusplus
}
#endif
