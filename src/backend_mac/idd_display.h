/*
 * idd_display -- NSWindowController hosting a Windows-guest IDD desktop over ivshmem.
 *
 * The Windows-guest peer of VzDisplayWindow. A VZ guest renders into an in-process
 * VZVirtualMachineView; a Windows guest has no such view, so this controller
 * reconstructs the guest's framebuffer from the VDD's ch2 (DISPLAY) wire stream
 * and sends host input over ch3 (INPUT), both carried on the same ivshmem
 * transport the agent/ssh/clipboard helpers use. The result is an interactive
 * Windows desktop in a real AppKit window.
 *
 * It also owns the guest's AUDIO (ch4) and CLIPBOARD (ch5 Mac->Win writer +
 * ch6 Win->Mac reader) host consumers, tied to the same open/close lifecycle:
 * opening the window starts those reader threads (playback via CoreAudio,
 * NSPasteboard <-> Windows clipboard sync), and closing it tears them down.
 * The ring reads/writes go through blocking fd I/O over -connectChannel:timeoutMs:.
 *
 * Mirrors VzDisplayWindow's surface (-initWith.../-showDisplay/-window/userClosed)
 * so asb_core_mac's g_display_refs[] and the headless daemon treat both uniformly.
 */

#import <Cocoa/Cocoa.h>

@class AsbIvshmemTransport;

@interface IddDisplayWindow : NSWindowController

/* Set in windowWillClose: so callers can tell a user-closed window (X) from a live
   one without touching AppKit off the main thread. Mirrors VzDisplayWindow; the
   headless daemon reads it (via the VzDisplayWindow-typed g_vms[].display) to
   report displayOpen and to reap a self-closed window before reopening. */
@property (nonatomic) BOOL userClosed;

/* `transport` is the VM's ivshmem transport (QemuVm.transport), available once the
   VM is Running. `name` titles the window. The controller starts a ch2 reader
   thread that connects + reconstructs frames, and writes ch3 input on demand. */
- (instancetype)initWithName:(NSString *)name
                   transport:(AsbIvshmemTransport *)transport;
- (void)showDisplay;
- (void)toggleBorderlessFullscreen:(id)sender;

@end
