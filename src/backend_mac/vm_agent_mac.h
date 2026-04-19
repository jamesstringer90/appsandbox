/*
 * vm_agent_mac -- Host-side connector for the AppSandbox guest agent.
 *
 * Mirrors backend_win/vm_agent.c: connects over a virtio-vsock channel
 * (VZVirtioSocketDevice on the host, AF_VSOCK listener in the guest),
 * reads "hello" to flip online state, tracks heartbeats, handles async
 * messages, and supports tagged commands (ping/shutdown/restart).
 *
 * Port 1 matches the low 16 bits of the Windows service GUID
 * (a5b0cafe-0001-...) for symmetry across platforms.
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

NS_ASSUME_NONNULL_BEGIN

#define VM_AGENT_MAC_PORT 1

typedef void (^VmAgentOnlineChange)(BOOL online);
typedef void (^VmAgentLog)(NSString *line);
/* Mirrors Windows ssh_state: 0=disabled, 1=installing, 2=ready, 3=failed. */
typedef void (^VmAgentSshStateChange)(int state);

@interface VmAgentMac : NSObject

@property (nonatomic, copy, readonly)     NSString *vmName;
@property (nonatomic, assign, readonly)   BOOL online;
@property (nonatomic, assign, readonly)   uint64_t lastHeartbeatMs;

/* Fires on main queue when online flips. */
@property (nonatomic, copy, nullable)     VmAgentOnlineChange onOnlineChange;

/* Fires on main queue for agent `log:` messages and state transitions. */
@property (nonatomic, copy, nullable)     VmAgentLog onLog;

/* Set before calling start: if YES, after receiving "hello" we send a
 * tagged ssh_enable command and report state transitions via
 * onSshStateChange. */
@property (nonatomic, assign)             BOOL sshEnabled;

/* Fires on main queue when the guest-reported ssh state changes
 * (synchronous reply to ssh_enable + any async ssh_ready/ssh_failed). */
@property (nonatomic, copy, nullable)     VmAgentSshStateChange onSshStateChange;

- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device;

/* Start the persistent connection thread. Safe to call once. */
- (void)start;

/* Stop, tear down the connection, invoke online-change(NO) if needed. */
- (void)stop;

/* Send a tagged command and wait up to timeoutSec for the "ok"/other reply.
 * Returns the raw response string (without the tag prefix), or nil on error. */
- (nullable NSString *)sendCommand:(NSString *)cmd
                           timeout:(NSTimeInterval)timeoutSec;

@end

NS_ASSUME_NONNULL_END
