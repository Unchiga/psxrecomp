#ifndef PSX_HOST_INPUT_H
#define PSX_HOST_INPUT_H

/* Injection of synthetic HOST input (as opposed to guest pad input, which is
 * what set_input/clear_input drive). Events are pushed into the SDL queue, so
 * they travel the exact same path as a physical click or keypress: the same
 * window->drawable coordinate scaling, the same event branch, the same menu
 * handlers. Nothing is special-cased for injected input.
 *
 * This exists because host-level synthesis (SendInput/keybd_event) is not a
 * reliable way to test the overlay UI from outside the process -- mouse motion
 * arrives but button presses do not -- which makes "is the dropdown open?"
 * untestable without it.
 *
 * SDL_PushEvent is documented thread-safe, so the debug server thread may call
 * these directly; the main loop dequeues them on its next poll. */

#ifdef __cplusplus
extern "C" {
#endif

/* Coordinates are in WINDOW pixels, matching a real SDL_MOUSEBUTTONDOWN.
 * Returns 1 when the event was queued. */
int psx_host_inject_mouse_click(int win_x, int win_y);
int psx_host_inject_mouse_move(int win_x, int win_y);

/* key is an SDL_Keycode. Returns 1 when the event was queued. */
int psx_host_inject_key(int sdl_keycode);

/* Writes SDL's joystick/gamepad enumeration plus per-player routing into out
 * as JSON object members (no enclosing braces). Returns bytes written. */
int psx_host_pad_devices_json(char *out, int cap);

/* Live host-side pad mask for a slot, PSX format (0 = pressed). Compare with
 * the SIO-visible mask to measure the emulator's own input latency. */
uint16_t psx_host_pad_mask_for_slot(int slot);

/* In-process input-latency samples (press visible to host -> the frame the
 * guest rendered from it is on screen), as JSON object members. Polling the
 * debug server cannot measure this: requests are serviced on the emu thread,
 * so a probe reads host and guest state at the same instant. */
int  psx_host_lag_json(char *out, int cap);
void psx_host_lag_reset(void);

/* Flip [video] geometry_correction / perspective_texturing while running, so
 * the same scene can be captured off vs on without a restart (the comparison
 * ENHANCEMENTS.md G1.6 requires). -1 leaves a knob unchanged. */
void psx_host_set_geometry_enhancements(int geometry, int perspective);

/* GAME > FAST LOADING level (0 off / 1 fast / 2 instant), so a load can be
 * timed at each step from a script. Returns the disc divisor now in force. */
int  psx_host_set_fast_loads(int level);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HOST_INPUT_H */
