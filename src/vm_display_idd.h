#ifndef VM_DISPLAY_IDD_H
#define VM_DISPLAY_IDD_H

#include <windows.h>
#include "hcs_vm.h"

typedef struct VmDisplayIdd VmDisplayIdd;

/* Create IDD frame display window for VM. Connects to VM's frame channel
   (AF_HYPERV service GUID :0002) and renders received frames via D3D11.
   main_hwnd receives WM_VM_DISPLAY_CLOSED when closed. */
VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

void vm_display_idd_destroy(VmDisplayIdd *display);
BOOL vm_display_idd_is_open(VmDisplayIdd *display);

#endif /* VM_DISPLAY_IDD_H */
