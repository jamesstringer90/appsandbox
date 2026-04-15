#ifndef VM_DISPLAY_H
#define VM_DISPLAY_H

#include <windows.h>
#include "hcs_vm.h"

/* Display context for a VM viewer (RDP ActiveX over named pipe). */
typedef struct VmDisplay VmDisplay;

/* Create and show a display window for the given VM.
   Connects to the VM's RDP pipe (\\.\pipe\<name>.BasicSession).
   main_hwnd receives WM_VM_DISPLAY_CLOSED when the user closes the window.
   Returns a display context, or NULL on failure. */
VmDisplay *vm_display_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

/* Close the display window and free resources. */
void vm_display_destroy(VmDisplay *display);

/* Check if the display window is still open. */
BOOL vm_display_is_open(VmDisplay *display);

/* Disconnect the RDP session and terminate the pipe connector.
   Call when the VM is exiting so the RDP session doesn't block shutdown. */
void vm_display_disconnect(VmDisplay *display);

/* Switch to enhanced session mode and reconnect.
   No-op if already in enhanced mode. */
void vm_display_set_enhanced(VmDisplay *display);

#endif /* VM_DISPLAY_H */
