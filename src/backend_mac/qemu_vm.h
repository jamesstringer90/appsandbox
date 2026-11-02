/*
 * qemu_vm.h -- macOS QEMU+HVF backend for a Windows guest (the VzVm peer).
 *
 * AppSandbox on Mac launches Windows VMs by driving the vendored QEMU (HVF acceleration, EDK2
 * firmware) instead of Virtualization.framework. The guest talks to the host over ivshmem shared
 * memory (no vsock), so QemuVm:
 *   1. creates + sizes the ivshmem backing file and PUBLISHES the asb_transport directory into it
 *      (asb_shm_publish) BEFORE launching QEMU, so guest services select ivshmem at boot;
 *   2. launches qemu-system-aarch64 as a child process (NVMe disk.img, vmnet NAT, -display none,
 *      ivshmem-plain + memory-backend-file, guest Secure Boot off in testMode, NO GPU);
 *   3. maps the backing as an AsbIvshmemTransport the channel helpers use (agent/ssh/clipboard);
 *   4. tracks the child + QMP and reports lifecycle transitions.
 *
 * Mirrors how Windows HCS defines the VM (hcs_vm.c), minus GPU-PV and with the HvSocket ServiceTable
 * replaced by the ivshmem device.
 */
#import <Foundation/Foundation.h>

@class AsbIvshmemTransport;

NS_ASSUME_NONNULL_BEGIN

/* Lifecycle states the core consumes (parallel to the VZVirtualMachineState values the macOS-guest
 * path uses, so asb_core_mac can treat both backends uniformly). */
typedef NS_ENUM(int, QemuVmState) {
    QemuVmStateStopped  = 0,
    QemuVmStateStarting = 1,
    QemuVmStateRunning  = 2,
    QemuVmStateStopping = 3,
};

typedef void (^QemuVmStateChange)(QemuVmState state);
typedef void (^QemuVmLog)(NSString *line);

@interface QemuVm : NSObject

+ (BOOL)nestedVirtualizationSupported;

/* Paths/params for one VM. ramMb/cpuCores/diskGb mirror the CoreVmConfig. The backing file lives in
 * the VM bundle (vmDir/ivshmem.bin); disk.img is the engine-built raw NTFS/GPT image. */
- (instancetype)initWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                       ramMb:(int)ramMb
                    cpuCores:(int)cpuCores
                  macAddress:(NSString *)macAddress
                    testMode:(BOOL)testMode
        nestedVirtualization:(BOOL)nestedVirtualization;

@property (nonatomic, copy, readonly)   NSString *name;
@property (nonatomic, assign, readonly) QemuVmState state;
@property (nonatomic, assign, readonly) BOOL exitedCleanly;

/* The shared-memory transport, available once the VM is Running. The core hands this to
 * VmAgentMac / VmSshProxyMac / VmClipboardMac (the ivshmem path). nil before start / after stop. */
@property (nonatomic, strong, readonly, nullable) AsbIvshmemTransport *transport;

@property (nonatomic, copy, nullable) QemuVmStateChange onStateChange;   /* fires on main queue */
@property (nonatomic, copy, nullable) QemuVmLog onLog;

/* Publish the directory, launch QEMU, map the transport. completion fires (main queue) with nil on a
 * successful launch or an error if QEMU couldn't start. */
- (void)startWithCompletion:(void (^_Nullable)(NSError *_Nullable))completion;

/* Graceful: QMP system_powerdown (ACPI). The core prefers the agent `shutdown` command first and
 * uses this as the fallback, mirroring asb_mac_vm_stop. */
- (void)requestStop;

/* Force: QMP quit, then terminate the child if it lingers. */
- (void)stop;

NS_ASSUME_NONNULL_END

@end
