/*
 * vm_ssh_proxy_mac -- Host-side TCP-to-vsock SSH proxy.
 *
 * Mirrors backend_win/vm_ssh_proxy.c: when a VM reports ssh_ready, we
 * bind 127.0.0.1:<ephemeral> (or the persisted ssh_port) and relay TCP
 * bytes to the guest agent's SSH listener on virtio-vsock port 7, which
 * in turn relays to the guest's sshd on 127.0.0.1:22.
 *
 * Usage from the outside world: `ssh user@127.0.0.1 -p <ssh_port>`.
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

NS_ASSUME_NONNULL_BEGIN

/* The vsock port the guest agent listens on for SSH relay. Matches the
 * low 16 bits of the Windows service GUID (a5b0cafe-0007-...). */
#define VM_SSH_PROXY_VSOCK_PORT 7

typedef void (^VmSshProxyPortChange)(int port);
typedef void (^VmSshProxyLog)(NSString *line);

@interface VmSshProxyMac : NSObject

@property (nonatomic, copy, readonly)  NSString *vmName;
@property (nonatomic, assign, readonly) int       port;  /* host loopback port, 0 if not yet bound */

/* Fires on main queue when we successfully bind a port (for persistence). */
@property (nonatomic, copy, nullable)  VmSshProxyPortChange onPortAssigned;

/* Fires on main queue for diagnostic events (listen/bind/accept/relay). */
@property (nonatomic, copy, nullable)  VmSshProxyLog        onLog;

- (instancetype)initWithName:(NSString *)vmName
                socketDevice:(VZVirtioSocketDevice *)device
                 initialPort:(int)preferredPort;

/* Bind loopback + start accept loop. Safe to call once. */
- (void)start;

/* Signal stop + close listener + wait for relay threads. */
- (void)stop;

@end

NS_ASSUME_NONNULL_END
