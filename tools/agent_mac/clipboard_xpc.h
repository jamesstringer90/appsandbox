/*
 * clipboard_xpc -- Shared XPC contract between the root-owned guest agent
 * (appsandbox-agent, LaunchDaemon) and the user-session clipboard helper
 * (appsandbox-clipboard, LaunchAgent).
 *
 * Why XPC at all: Darwin's AF_VSOCK bind requires root for any port, and
 * NSPasteboard requires a user GUI (Aqua) session. The root daemon binds
 * vsock :5, and each accepted connection is handed to the user helper as
 * an NSFileHandle over XPC. The helper speaks the clipboard byte protocol
 * (FORMAT_LIST / DATA_REQ / DATA_RESP / FILE_DATA) directly on that fd —
 * no proxying in the daemon.
 *
 * Direction: helper connects outbound to the daemon's Mach service
 * (user → system is the easy direction; system → user session is not).
 * Once connected, the daemon keeps a weak handle to the helper's exported
 * callback object and invokes -takeHostConnection: on every accept.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* Registered by the daemon via its LaunchDaemon plist MachServices key. */
#define CLIPBOARD_XPC_MACH_SERVICE @"com.appsandbox.agent.clipboard"

/* Daemon's listener-side interface. Helper calls this once on connect. */
@protocol AppSandboxClipboardService <NSObject>
- (void)helperReady;
@end

/* Helper's callback interface. Daemon invokes this per accepted vsock. */
@protocol AppSandboxClipboardCallback <NSObject>
- (void)takeHostConnection:(NSFileHandle *)connection;
@end

NS_ASSUME_NONNULL_END
