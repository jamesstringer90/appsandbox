#import "qemu_vm.h"
#import "vm_dir.h"
#import "asb_ivshmem_transport.h"
#import "asb_network_mac.h"
#import <Hypervisor/Hypervisor.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../tools/transport/asb_shm_layout.h"   /* asb_shm_publish + region sizing */

#define QEMU_IVSHMEM_SIZE  (128ull * 1024 * 1024)

@interface QemuVm ()
@property (nonatomic, copy)   NSString *name;
@property (nonatomic, strong) NSURL *vmDir;
@property (nonatomic, assign) int ramMb;
@property (nonatomic, assign) int cpuCores;
@property (nonatomic, copy) NSString *macAddress;
@property (nonatomic, assign) BOOL testMode;
@property (nonatomic, assign) BOOL nestedVirtualization;
@property (nonatomic, assign, readwrite) QemuVmState state;
@property (nonatomic, strong, readwrite) AsbIvshmemTransport *transport;
@property (nonatomic, strong) NSTask *task;          /* set only on the root NSTask path */
@property (nonatomic, strong) AsbNatPort *networkPort;
@property (nonatomic, assign) int monitorPort;       /* loopback HMP monitor (TCP) */
@end

@implementation QemuVm

+ (BOOL)nestedVirtualizationSupported {
    bool supported = false;
    return hv_vm_config_get_el2_supported(&supported) == HV_SUCCESS && supported;
}

- (instancetype)initWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                       ramMb:(int)ramMb
                    cpuCores:(int)cpuCores
                  macAddress:(NSString *)macAddress
                    testMode:(BOOL)testMode
        nestedVirtualization:(BOOL)nestedVirtualization {
    if ((self = [super init])) {
        _name = [name copy];
        _vmDir = vmDir;
        _ramMb = ramMb > 0 ? ramMb : 4096;
        _cpuCores = cpuCores > 0 ? cpuCores : 4;
        _macAddress = [macAddress copy];
        _testMode = testMode;
        _nestedVirtualization = nestedVirtualization;
        _state = QemuVmStateStopped;
    }
    return self;
}

#pragma mark - Vendored QEMU + firmware resolution

/* Dev fallback root: walk up from the bundle to the repo's vendored dist (the staged,
 * self-contained qemu + gzipped firmware produced by stage.sh). Mirrors
 * windows_payload_directory. Returns nil in a shipped app, where everything is embedded
 * under Contents/Resources/qemu (the Embed QEMU build phase). */
- (nullable NSString *)devDistRoot {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *cur = [NSBundle mainBundle].bundlePath;
    for (int i = 0; i < 8 && cur.length > 1; i++) {
        NSString *cand = [cur stringByAppendingPathComponent:@"vendor/qemu-ivshmem/dist"];
        if ([fm fileExistsAtPath:[cand stringByAppendingPathComponent:@"bin/qemu-system-aarch64"]]) return cand;
        cur = [cur stringByDeletingLastPathComponent];
    }
    return nil;
}

/* Bundle first (Contents/Resources/qemu/<rel>), then the vendored dist. QEMU + EDK2 are the only
 * non-Xcode vendored deps; everything else is built by Xcode. */
- (nullable NSString *)resolveUnder:(NSString *)rel {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *bundled = [[[NSBundle mainBundle].resourcePath
                          stringByAppendingPathComponent:@"qemu"] stringByAppendingPathComponent:rel];
    if (bundled && [fm fileExistsAtPath:bundled]) return bundled;
    NSString *dist = [self devDistRoot];
    if (dist) {
        NSString *p = [dist stringByAppendingPathComponent:rel];
        if ([fm fileExistsAtPath:p]) return p;
    }
    return nil;
}

- (nullable NSString *)qemuBinary {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *bundled = [[[NSBundle mainBundle].resourcePath
                          stringByAppendingPathComponent:@"qemu"] stringByAppendingPathComponent:@"bin/qemu-system-aarch64"];
    if ([fm fileExistsAtPath:bundled]) return bundled;
    NSString *dist = [self devDistRoot];
    if (dist) {
        NSString *p = [dist stringByAppendingPathComponent:@"bin/qemu-system-aarch64"];
        if ([fm fileExistsAtPath:p]) return p;
    }
    return nil;
}

/* Decompress a gzip file to dest via the base-system /usr/bin/gunzip — present on every macOS
 * install with zero developer tools. Used for the gzipped EDK2 firmware. */
- (BOOL)gunzip:(NSString *)gz to:(NSString *)dest error:(NSError **)err {
    [[NSFileManager defaultManager] createFileAtPath:dest contents:nil attributes:nil];
    NSFileHandle *fh = [NSFileHandle fileHandleForWritingAtPath:dest];
    if (!fh) { if (err) *err = [self err:@"cannot open firmware output"]; return NO; }
    NSTask *t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/gunzip"];
    t.arguments = @[ @"-c", gz ];
    t.standardOutput = fh;
    NSError *runErr = nil;
    if (![t launchAndReturnError:&runErr]) { [fh closeFile]; if (err) *err = runErr; return NO; }
    [t waitUntilExit];
    [fh closeFile];
    if (t.terminationStatus != 0) { if (err) *err = [self err:@"gunzip failed"]; return NO; }
    return YES;
}

#pragma mark - Lifecycle

- (void)setStateOnMain:(QemuVmState)st {
    self.state = st;
    QemuVmStateChange cb = self.onStateChange;
    if (cb) dispatch_async(dispatch_get_main_queue(), ^{ cb(st); });
}

- (void)logFmt:(NSString *)fmt, ... {
    va_list ap; va_start(ap, fmt);
    NSString *msg = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    QemuVmLog cb = self.onLog;
    if (cb) dispatch_async(dispatch_get_main_queue(), ^{ cb(msg); });
}

/* Create + size the ivshmem backing and publish the directory into it before QEMU maps it. */
- (BOOL)prepareIvshmemBacking:(NSString *)path error:(NSError **)err {
    int fd = open(path.fileSystemRepresentation, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { if (err) *err = [self posixError:@"open ivshmem.bin"]; return NO; }
    if (ftruncate(fd, (off_t)QEMU_IVSHMEM_SIZE) != 0) {
        int e = errno; close(fd);
        if (err) *err = [self err:[NSString stringWithFormat:@"ftruncate ivshmem.bin: %s", strerror(e)]];
        return NO;
    }
    void *bar = mmap(NULL, (size_t)QEMU_IVSHMEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) { close(fd); if (err) *err = [self posixError:@"mmap ivshmem.bin"]; return NO; }
    asb_shm_publish((uint8_t *)bar, QEMU_IVSHMEM_SIZE);
    msync(bar, 4096, MS_SYNC);
    munmap(bar, (size_t)QEMU_IVSHMEM_SIZE);
    close(fd);
    return YES;
}

- (void)startWithCompletion:(void (^)(NSError *_Nullable))completion {
    void (^done)(NSError *) = ^(NSError *e) {
        if (completion) dispatch_async(dispatch_get_main_queue(), ^{ completion(e); });
    };

    NSString *qemu = [self qemuBinary];
    if (!qemu) { done([self err:@"vendored qemu-system-aarch64 not found"]); return; }
    /* Firmware ships gzipped (edk2-*.fd.gz, ~1.6 MB) and is decompressed with the base-system
     * /usr/bin/gunzip (zero developer-tool deps): the read-only code firmware once into a shared
     * cache, the writable vars template per-VM (below). */
    NSString *codeGz = [self resolveUnder:@"share/qemu/edk2-aarch64-code.fd.gz"];
    NSString *varsGz = [self resolveUnder:@"share/qemu/edk2-arm-vars.fd.gz"];
    if (!codeGz || !varsGz) { done([self err:@"vendored EDK2 firmware (.gz) not found"]); return; }

    NSFileManager *fmEdk = [NSFileManager defaultManager];
    NSString *fwDir = [[[[self.vmDir URLByDeletingLastPathComponent]   /* .../VMs */
                          URLByDeletingLastPathComponent]               /* .../<app support root> */
                         URLByAppendingPathComponent:@"firmware"] path];
    [fmEdk createDirectoryAtPath:fwDir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *shareDir = fwDir;
    NSString *edk2Code = [fwDir stringByAppendingPathComponent:@"edk2-aarch64-code.fd"];
    if (![fmEdk fileExistsAtPath:edk2Code]) {
        NSError *gzErr = nil;
        if (![self gunzip:codeGz to:edk2Code error:&gzErr]) {
            done(gzErr ?: [self err:@"decompress EDK2 code firmware"]); return;
        }
    }

    NSString *diskPath = [VmDir diskImageURLFor:self.name].path;
    NSString *ivsPath  = [self.vmDir URLByAppendingPathComponent:@"ivshmem.bin"].path;
    NSString *varsPath = [self.vmDir URLByAppendingPathComponent:@"vars.fd"].path;

    if (![[NSFileManager defaultManager] fileExistsAtPath:diskPath]) {
        done([self err:@"disk.img not found (build it first)"]); return;
    }

    /* Per-VM writable NVRAM vars. Seed once from the EDK2 template, then PERSIST across boots (boot
     * entries the firmware writes must survive). testMode: Secure Boot stays off — our VDD/ivshmem/
     * VAD drivers are test-signed; no vTPM on ARM Windows. */
    if (![[NSFileManager defaultManager] fileExistsAtPath:varsPath]) {
        NSError *gzErr = nil;
        if (![self gunzip:varsGz to:varsPath error:&gzErr]) {
            done(gzErr ?: [self err:@"decompress EDK2 vars template"]);
            return;
        }
    }

    NSError *ivErr = nil;
    if (![self prepareIvshmemBacking:ivsPath error:&ivErr]) { done(ivErr); return; }

    self.monitorPort = [self allocLoopbackPort];
    if (self.monitorPort <= 0) { done([self err:@"could not allocate a monitor port"]); return; }

    if (self.nestedVirtualization && ![QemuVm nestedVirtualizationSupported]) {
        done([self err:@"This Mac does not support nested virtualization for Windows guests."]);
        return;
    }
    NSString *accelerator = self.nestedVirtualization ? @"hvf,kernel-irqchip=on" : @"hvf";
    NSString *machine = self.nestedVirtualization
        ? @"virt,gic-version=3,highmem=on,virtualization=on"
        : @"virt,gic-version=3,highmem=on";
    if (self.nestedVirtualization)
        [self logFmt:@"Nested virtualization enabled; QEMU disables SME for this CPU configuration."];

    NSArray<NSString *> *args = @[
        @"-name", self.name,
        @"-accel", accelerator, @"-cpu", @"host",
        @"-M", machine,
        @"-smp", [NSString stringWithFormat:@"%d", self.cpuCores],
        @"-m", [NSString stringWithFormat:@"%d", self.ramMb],
        @"-L", shareDir,
        @"-drive", [NSString stringWithFormat:@"if=pflash,format=raw,readonly=on,file=%@", edk2Code],
        @"-drive", [NSString stringWithFormat:@"if=pflash,format=raw,file=%@", varsPath],
        @"-object", [NSString stringWithFormat:@"memory-backend-file,id=ivshm,size=%llu,mem-path=%@,share=on",
                     QEMU_IVSHMEM_SIZE, ivsPath],
        @"-device", @"ivshmem-plain,memdev=ivshm",
        /* No ramfb: with -display none nothing renders QEMU's framebuffer anyway, and a second guest
         * display adapter competes with the VDD for the desktop/primary path (we have no
         * SetDisplayConfig topology code). Dropping it leaves the VDD as the sole adapter, mirroring
         * the Hyper-V case where the desktop lands on the VDD with no topology setup. */
        @"-device", @"qemu-xhci,id=usb", @"-device", @"usb-kbd", @"-device", @"usb-tablet",
        @"-device", @"nvme,drive=hdd,serial=asb-nvme,bootindex=0",
        @"-drive", [NSString stringWithFormat:@"if=none,id=hdd,format=raw,file=%@",
                    [diskPath stringByReplacingOccurrencesOfString:@"," withString:@",,"]],
        @"-netdev", @"stream,id=net0,server=off,addr.type=fd,addr.str=0",
        @"-device", [NSString stringWithFormat:@"virtio-net-pci,netdev=net0,romfile=,mac=%@", self.macAddress],
        @"-display", @"none",
        @"-rtc", @"base=localtime",
        @"-monitor", [NSString stringWithFormat:@"tcp:127.0.0.1:%d,server,nowait", self.monitorPort],
    ];

    [self setStateOnMain:QemuVmStateStarting];

    if (geteuid() != 0) {
        [self setStateOnMain:QemuVmStateStopped];
        done([self err:@"The AppSandbox core must start with administrator privileges."]);
        return;
    }
    NSError *networkError = nil;
    AsbNatNetwork *network = [AsbNatNetwork sharedNetworkWithError:&networkError];
    AsbNatPort *port = network ? [[AsbNatPort alloc] initWithNetwork:network name:self.name error:&networkError] : nil;
    if (!port) {
        [self setStateOnMain:QemuVmStateStopped];
        done(networkError);
        return;
    }
    self.networkPort = port;
    NSMutableArray<NSString *> *launchArgs = [args mutableCopy];
    NSString *resources = [self.vmDir URLByAppendingPathComponent:@"resources.iso"].path;
    if ([[NSFileManager defaultManager] fileExistsAtPath:resources]) {
        [launchArgs addObjectsFromArray:@[@"-device", @"usb-storage,drive=setup",
            @"-drive", [NSString stringWithFormat:@"if=none,id=setup,media=cdrom,readonly=on,format=raw,file=%@",
                       [resources stringByReplacingOccurrencesOfString:@"," withString:@",,"]]]];
    }
    uid_t owner = [VmDir ownerUID];
    NSString *runWith = owner ? [NSString stringWithFormat:@"user=%u:%u,exit-with-parent=on",
        (unsigned)owner, (unsigned)[VmDir ownerGID]] : @"exit-with-parent=on";
    [launchArgs addObjectsFromArray:@[@"-run-with", runWith]];
    [port startWithCompletion:^(NSError *attachError) {
        if (self.networkPort != port) { [port stop]; return; }
        if (self.state != QemuVmStateStarting || attachError) {
            [port stop];
            self.networkPort = nil;
            [self setStateOnMain:QemuVmStateStopped];
            done(attachError ?: [self err:@"VM start cancelled"]);
            return;
        }
        NSTask *task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath:qemu];
        task.arguments = launchArgs;
        task.environment = @{ @"PATH": @"/usr/bin:/bin:/usr/sbin:/sbin", @"LANG": @"en_US.UTF-8" };
        __weak QemuVm *weakSelf = self;
        task.terminationHandler = ^(NSTask *t) {
            [port stop];
            QemuVm *s = weakSelf;
            if (!s) return;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (s.task != t) return;
                [s.transport close];
                s.transport = nil;
                s.networkPort = nil;
                s.task = nil;
                s->_exitedCleanly = t.terminationStatus == 0;
                [s logFmt:@"[%@] QEMU exited (status %d)", s.name, t.terminationStatus];
                [s setStateOnMain:QemuVmStateStopped];
            });
        };
        NSError *launchErr = nil;
        self.task = task;
        if (![port launchTask:task error:&launchErr]) {
            self.task = nil;
            [port stop];
            self.networkPort = nil;
            [self setStateOnMain:QemuVmStateStopped];
            done(launchErr ?: [self err:@"qemu launch failed"]);
            return;
        }
        __weak AsbNatPort *weakPort = port;
        port.onFailure = ^(NSError *error) {
            QemuVm *s = weakSelf;
            if (!s || s.networkPort != weakPort || !s.task.isRunning) return;
            [s logFmt:@"[%@] Shared NAT failed: %@", s.name, error.localizedDescription];
            [s stop];
        };
        [self logFmt:@"[%@] QEMU launched by core (pid %d, guest execution uid %u)",
                      self.name, task.processIdentifier, (unsigned)owner];

        /* Map the published backing as the transport the channel helpers use. */
        self.transport = [[AsbIvshmemTransport alloc] initWithBackingPath:ivsPath];
        if (!self.transport) { [self logFmt:@"[%@] WARN: transport map failed", self.name]; }

        [self setStateOnMain:QemuVmStateRunning];
        done(nil);
    }];
}

/* Bind a loopback TCP socket to port 0, read the kernel-assigned port, release it, and hand that
 * port to QEMU's -monitor. (Tiny TOCTOU window on loopback; acceptable for a control socket.) */
- (int)allocLoopbackPort {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    int port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0) {
        socklen_t l = sizeof a;
        if (getsockname(s, (struct sockaddr *)&a, &l) == 0) port = ntohs(a.sin_port);
    }
    close(s);
    return port;
}

#pragma mark - Stop

/* Send a one-shot HMP command over the QEMU monitor (loopback TCP, so it works regardless of
 * whether QEMU is running as us or as root via AEWP). */
- (BOOL)sendMonitor:(NSString *)cmd {
    if (self.monitorPort <= 0) return NO;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NO;
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)self.monitorPort);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return NO; }
    NSString *line = [cmd stringByAppendingString:@"\n"];
    const char *c = line.UTF8String;
    BOOL ok = send(s, c, strlen(c), 0) == (ssize_t)strlen(c);
    close(s);
    return ok;
}

- (void)requestStop {
    if (self.state != QemuVmStateRunning) return;
    [self setStateOnMain:QemuVmStateStopping];
    [self logFmt:@"[%@] ACPI power-down (monitor system_powerdown)", self.name];
    [self sendMonitor:@"system_powerdown"];   /* guest sees the power button; SystemExited via terminationHandler */
}

- (void)stop {
    if (self.state == QemuVmStateStarting && !self.task) {
        [self.networkPort stop];
        self.networkPort = nil;
        [self setStateOnMain:QemuVmStateStopped];
        return;
    }
    [self logFmt:@"[%@] force stop (monitor quit)", self.name];
    if (![self sendMonitor:@"quit"]) {
        @try { [self.task terminate]; } @catch (__unused NSException *e) {}
    }
}

- (void)dealloc { [_networkPort stop]; }

#pragma mark - Errors

- (NSError *)err:(NSString *)msg {
    return [NSError errorWithDomain:@"QemuVm" code:1 userInfo:@{NSLocalizedDescriptionKey: msg}];
}
- (NSError *)posixError:(NSString *)what {
    return [NSError errorWithDomain:NSPOSIXErrorDomain code:errno
                          userInfo:@{NSLocalizedDescriptionKey:
                                     [NSString stringWithFormat:@"%@: %s", what, strerror(errno)]}];
}

@end
