/*
 * asb_display.h -- Headless IDD display API.
 *
 * Connects to a running VM's AF_HYPERV frame and input channels
 * without creating a window or D3D11 device.  Provides:
 *   - Screenshot capture (raw BGRA pixels)
 *   - Keyboard and mouse input injection
 *
 * This is the programmatic equivalent of the IDD display window
 * (vm_display_idd.c), designed for automation and scripting.
 */

#ifndef ASB_DISPLAY_H
#define ASB_DISPLAY_H

#include <windows.h>
#include "asb_core.h"

/* ---- Display handle ---- */

/* Opaque handle — defined in asb_display.c */
/* AsbDisplay is forward-declared in asb_core.h */

/* ---- Mouse button IDs ---- */

#define ASB_BTN_LEFT    0
#define ASB_BTN_RIGHT   1
#define ASB_BTN_MIDDLE  2

/* ---- API ---- */

/* Connect to a running VM's frame and input channels.
   Returns NULL on failure (VM not running, agent not ready, etc.).
   The display receives frames in the background; call asb_display_screenshot()
   to capture the latest frame. */
ASB_API AsbDisplay *asb_display_connect(AsbVm vm);

/* Disconnect and free the display handle.
   Safe to call with NULL. */
ASB_API void asb_display_disconnect(AsbDisplay *disp);

/* Check if the display is still connected and receiving frames. */
ASB_API BOOL asb_display_is_connected(AsbDisplay *disp);

/* Capture the current frame as raw BGRA pixels.
   buf_out: caller-allocated buffer, or NULL to query dimensions only.
   width/height/stride: set to current frame dimensions on return.
   Returns TRUE if a frame was captured, FALSE if no frame available yet.

   Usage pattern:
     UINT w, h, s;
     asb_display_screenshot(disp, NULL, &w, &h, &s);  // query size
     BYTE *buf = malloc(s * h);
     asb_display_screenshot(disp, buf, &w, &h, &s);   // capture */
ASB_API BOOL asb_display_screenshot(AsbDisplay *disp, BYTE *buf_out,
                                     UINT *width, UINT *height, UINT *stride);

/* ---- Mouse input ---- */

/* Move mouse to absolute VM coordinates (0,0 = top-left of framebuffer). */
ASB_API void asb_display_mouse_move(AsbDisplay *disp, UINT x, UINT y);

/* Click (press + release) a mouse button at the given position. */
ASB_API void asb_display_mouse_click(AsbDisplay *disp, UINT x, UINT y, int button);

/* Send mouse wheel event.  delta > 0 = scroll up, delta < 0 = scroll down. */
ASB_API void asb_display_mouse_wheel(AsbDisplay *disp, int delta);

/* Press or release a mouse button.  down: TRUE = press, FALSE = release. */
ASB_API void asb_display_mouse_button(AsbDisplay *disp, UINT x, UINT y,
                                       int button, BOOL down);

/* ---- Keyboard input ---- */

/* Press or release a key by virtual key code. */
ASB_API void asb_display_key(AsbDisplay *disp, UINT vk, BOOL down);

/* Type a string by sending key press/release pairs for each character.
   Handles shift for uppercase and common symbols. */
ASB_API void asb_display_type_text(AsbDisplay *disp, const wchar_t *text);

#endif /* ASB_DISPLAY_H */
