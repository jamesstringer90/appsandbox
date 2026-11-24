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

@class AsbIvshmemTransport;

NS_ASSUME_NONNULL_BEGIN

#define VM_AGENT_MAC_PORT 1     /* == ASB_CH_AGENT for the ivshmem path */

typedef void (^VmAgentOnlineChange)(BOOL online, uint32_t version);
typedef void (^VmAgentLog)(NSString *line);
/* Mirrors Windows ssh_state: 0=disabled, 1=installing, 2=ready, 3=failed. */
typedef void (^VmAgentSshStateChange)(int state);
/* Fires on main queue with the guest's ssh_key_deployed / ssh_key_failed reply. */
typedef void (^VmAgentKeyDeployed)(BOOL ok);
/* Fires on main queue from the guest's idd_status: line (VDD driver state). Windows guest only;
 * gates display_ready (= running && agentOnline && iddReady), mirroring Windows asb_vm_idd_ready. */
typedef void (^VmAgentIddStatus)(BOOL ready);

@interface VmAgentMac : NSObject

@property (nonatomic, copy, readonly)     NSString *vmName;
@property (nonatomic, assign, readonly)   BOOL online;
@property (atomic, assign, readonly)      NSInteger displayMonitorState;
@property (atomic, assign, readonly)      BOOL sasSupported;
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

/* Set before start: the AppSandbox public-key line to deploy, or nil. When
 * set, the agent sends `ssh_deploy_key <line>` FIRE-AND-FORGET as soon as SSH
 * is ready (mirrors Windows vm_agent.c) -- not via the blocking tagged-command
 * channel, whose 5 s socket-recv cap would drop a slow reply (the guest's
 * getpwent can be cold on a freshly booted VM). The async ssh_key_deployed /
 * ssh_key_failed reply fires onKeyDeployed. */
@property (nonatomic, copy, nullable)     NSString *deployKeyLine;
@property (nonatomic, copy, nullable)     VmAgentKeyDeployed onKeyDeployed;

/* Fires on main queue from the guest's idd_status: line (Windows guest / ivshmem path). */
@property (nonatomic, copy, nullable)     VmAgentIddStatus onIddStatusChange;
@property (nonatomic, copy, nullable)     dispatch_block_t onDiskResizeSupported;

/* VZ path (macOS guest): reach the agent over a VZVirtioSocketDevice. */
- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device;

/* ivshmem path (Windows guest): reach the agent over the shared-memory transport (ch1). The two
 * inits are mutually exclusive; the connection loop and protocol above the fd are identical. */
- (instancetype)initWithName:(NSString *)vmName
             ivshmemTransport:(AsbIvshmemTransport *)transport;

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
