#ifndef VM_CLIPBOARD_H
#define VM_CLIPBOARD_H

#include <winsock2.h>
#include <windows.h>

#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/*
 * vm_clipboard -- Per-VM clipboard synchronization over Hyper-V sockets.
 *
 * Manages both channels:
 *   :0005  Host→guest (clipboard writer) — sends FORMAT_LIST + serves DATA_REQ
 *   :0006  Guest→host (clipboard reader) — receives FORMAT_LIST + fetches data
 *
 * Focus-gated: sync is disabled by default. The display window enables it
 * on WM_SETFOCUS / restore and disables on WM_KILLFOCUS / minimize. When
 * disabled, no FORMAT_LIST is sent to the guest and incoming FORMAT_LIST
 * from the guest is discarded.
 */

/* Sync-enable message sent on :0005. Body: 1 byte (0=disable, 1=enable). */
#define CLIP_MSG_SYNC_ENABLE  12

typedef void (*VmClipboardLogFn)(const wchar_t *msg, void *user_data);

typedef struct VmClipboardData *VmClipboard;

/* Create a clipboard instance for a VM.
 * runtime_id: the VM's HCS runtime GUID (for HV socket connections).
 * hwnd: the display window that owns the clipboard listener.
 * log_fn/log_ud: optional logging callback (fires on same thread as event).
 * The instance starts with sync DISABLED — call set_sync_enabled after
 * the display window is ready to receive focus. */
ASB_API VmClipboard vm_clipboard_create(const GUID *runtime_id, HWND hwnd,
                                        VmClipboardLogFn log_fn, void *log_ud);

/* Tear down: close sockets, stop threads, free memory. */
ASB_API void vm_clipboard_destroy(VmClipboard clip);

/* Enable or disable clipboard sync.
 * On enable: sends SYNC_ENABLE(1) to guest, pushes current host clipboard.
 * On disable: sends SYNC_ENABLE(0) to guest, stops forwarding. */
ASB_API void vm_clipboard_set_sync_enabled(VmClipboard clip, BOOL enabled);

/* Call from WM_CLIPBOARDUPDATE in the display window's wndproc.
 * Forwards host clipboard to guest if sync is enabled. */
ASB_API void vm_clipboard_on_clipboard_update(VmClipboard clip);

/* Call from the display wndproc for WM_CLIP_READER_APPLY (WM_APP + 11).
 * Applies guest clipboard data that the reader thread accumulated. */
ASB_API void vm_clipboard_on_reader_apply(VmClipboard clip);

#endif /* VM_CLIPBOARD_H */
