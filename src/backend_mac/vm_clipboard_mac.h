/*
 * vm_clipboard_mac -- Host-side counterpart of appsandbox-clipboard.m.
 *
 * Per-VM persistent vsock connection to guest port 5 carrying the same
 * FORMAT_LIST / DATA_REQ / DATA_RESP / FILE_DATA protocol as the Windows
 * agent clipboard channel. Monitors host NSPasteboard.general via a
 * dispatch_source timer (macOS has no WM_CLIPBOARDUPDATE equivalent).
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

NS_ASSUME_NONNULL_BEGIN

/* Must match tools/agent_mac/appsandbox-agent.m CLIP_VSOCK_PORT.
 * The guest root agent binds this (AF_VSOCK bind is root-only on Darwin)
 * and hands each accepted connection to the user clipboard helper over
 * its XPC Mach service. */
#define VM_CLIPBOARD_VSOCK_PORT 5

typedef void (^VmClipboardLog)(NSString *line);

@interface VmClipboardMac : NSObject

@property (nonatomic, copy, readonly) NSString *vmName;

/* Fires on main queue for noteworthy events (connect/disconnect, FORMAT_LIST
 * send/recv, apply, errors). Not per-poll-tick. */
@property (nonatomic, copy, nullable)  VmClipboardLog onLog;

- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device;

- (void)start;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
