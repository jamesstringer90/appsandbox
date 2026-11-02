#ifndef UI_H
#define UI_H

#include <windows.h>

/* DLL export/import — matches asb_core.h */
#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/* Create and show the main application window.
   Returns the window handle, or NULL on failure. */
HWND ui_create_main_window(HINSTANCE hInstance, int nCmdShow);

/* Write a line to appsandbox.log and forward it to the registered
   AsbLogCallback (if any). Thread-safe: can be called from any thread.
   Implemented in asb_core.c (DLL). */
ASB_API void ui_log(const wchar_t *fmt, ...);

/* Get the HINSTANCE used to create the main window.
   Implemented in asb_core.c (DLL). */
ASB_API HINSTANCE ui_get_instance(void);

/* Save/load per-VM state JSON file (beside disk.vhdx).
   Implemented in asb_core.c (DLL). */
ASB_API void vm_save_state_json(const wchar_t *vhdx_path, BOOL install_complete);
ASB_API BOOL vm_load_state_json(const wchar_t *vhdx_path);

BOOL ui_command_active(void);
void ui_refresh_from_core(void);
BOOL ui_display_is_open(UINT64 id);
BOOL ui_display_open(UINT64 id);
void ui_display_close(UINT64 id);

#endif /* UI_H */
