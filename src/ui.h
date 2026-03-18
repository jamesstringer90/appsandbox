#ifndef UI_H
#define UI_H

#include <windows.h>

/* Create and show the main application window.
   Returns the window handle, or NULL on failure. */
HWND ui_create_main_window(HINSTANCE hInstance, int nCmdShow);

/* Append a line to the log output in the main window. */
void ui_log(const wchar_t *fmt, ...);

/* Get the HINSTANCE used to create the main window. */
HINSTANCE ui_get_instance(void);

/* Save/load per-VM state JSON file (beside disk.vhdx).
   vm_save_state_json writes {"installComplete":N} to <vhdx_dir>\vm_state.json.
   vm_load_state_json returns TRUE if installComplete is set in the file. */
void vm_save_state_json(const wchar_t *vhdx_path, BOOL install_complete);
BOOL vm_load_state_json(const wchar_t *vhdx_path);

#endif /* UI_H */
