#import "qemu_vm.h"
#import "vm_dir.h"
#import "asb_ivshmem_transport.h"
#import <Security/Security.h>

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

/* ivshmem backing size — must match the -object memory-backend-file size= below and be >= the
 * published region span. 128 MiB (matches the validated dev harness). */
#define QEMU_IVSHMEM_SIZE  (128ull * 1024 * 1024)

@interface QemuVm () {
    AuthorizationRef _authRef;     /* non-nil only on the elevated (AEWP) launch path */
    FILE            *_elevatedPipe; /* QEMU stdout when launched via AEWP; EOF == exit */
}
@property (nonatomic, copy)   NSString *name;
@property (nonatomic, strong) NSURL *vmDir;
@property (nonatomic, assign) int ramMb;
@property (nonatomic, assign) int cpuCores;
@property (nonatomic, assign) BOOL testMode;
@property (nonatomic, assign, readwrite) QemuVmState state;
@property (nonatomic, strong, readwrite) AsbIvshmemTransport *transport;
@property (nonatomic, strong) NSTask *task;          /* set only on the root NSTask path */
@property (nonatomic, assign) int monitorPort;       /* loopback HMP monitor (TCP) */
@end

@implementation QemuVm

- (instancetype)initWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                       ramMb:(int)ramMb
                    cpuCores:(int)cpuCores
                    testMode:(BOOL)testMode {
    if ((self = [super init])) {
        _name = [name copy];
        _vmDir = vmDir;
        _ramMb = ramMb > 0 ? ramMb : 4096;
        _cpuCores = cpuCores > 0 ? cpuCores : 4;
        _testMode = testMode;
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
    asb_shm_publish((uint8_t *)bar, QEMU_IVSHMEM_SIZE);   /* same publish the dev iso-patch-mac does */
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

    /* Stable per-VM MAC: locally-administered 52:54:00 OUI + 3 bytes of an FNV-1a hash of the VM
     * name. Concurrent VMs share the vmnet-shared L2 bridge, so they must not collide on QEMU's
     * fixed default NIC MAC (52:54:00:12:34:56). */
    uint32_t macHash = 2166136261u;
    for (const char *p = self.name.UTF8String; *p; p++) { macHash = (macHash ^ (uint8_t)*p) * 16777619u; }
    NSString *guestMac = [NSString stringWithFormat:@"52:54:00:%02x:%02x:%02x",
                          (macHash >> 16) & 0xff, (macHash >> 8) & 0xff, macHash & 0xff];

    /* argv mirrors the validated dev boot script, parameterized. No GPU; no swtpm/TPM (ARM Windows
     * HCS exposes none, hcs_vm.c); HvSocket replaced by ivshmem-plain; -display none (the VDD is the
     * display over ch2). Networking is vmnet-shared (Apple NAT): the guest gets a routable private IP
     * so the host can reach it on any port AND it gets internet. vmnet needs root -> obtained either
     * because we are the root daemon or via the elevation prompt below; there is NO fall back to
     * user-mode NAT (see [[windows-on-mac-networking]]). The HMP monitor is loopback TCP so an
     * unprivileged app can still control a root-owned QEMU. */
    NSArray<NSString *> *args = @[
        @"-name", self.name,
        @"-accel", @"hvf", @"-cpu", @"host",
        @"-M", @"virt,gic-version=3,highmem=on",
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
        @"-netdev", @"vmnet-shared,id=net0",
        /* NAT networking: vmnet-shared NATs the guest to the internet AND puts the host on the same
         * private subnet (bridge100 is the gateway), so host<->guest is reachable over IP. The NIC is
         * the standard QEMU virtio-net-pci (clean PCI, no USB stack); the guest binds the NetKVM driver
         * (virtio-win, w11/ARM64, WHQL-signed). virtio-net-pci is used rather than usb-net/RNDIS, which
         * Win11 ARM64 binds as a COM port instead of a NIC. Stable per-VM MAC for a consistent DHCP
         * lease on the shared bridge.
         * (see [[windows-on-mac-networking]].) */
        @"-device", [NSString stringWithFormat:@"virtio-net-pci,netdev=net0,romfile=,mac=%@", guestMac],
        @"-display", @"none",
        @"-rtc", @"base=localtime",
        @"-monitor", [NSString stringWithFormat:@"tcp:127.0.0.1:%d,server,nowait", self.monitorPort],
    ];

    [self setStateOnMain:QemuVmStateStarting];

    /* vmnet requires root. If we are already privileged (the headless daemon runs as root), launch
     * QEMU directly via NSTask. Otherwise (GUI app, unprivileged) launch it elevated through
     * AuthorizationExecuteWithPrivileges, which pops the macOS admin prompt. */
    if (geteuid() == 0) {
        NSTask *task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath:qemu];
        task.arguments = args;
        __weak QemuVm *weakSelf = self;
        task.terminationHandler = ^(NSTask *t) {
            QemuVm *s = weakSelf;
            if (!s) return;
            [s.transport close];
            s.transport = nil;
            [s logFmt:@"[%@] QEMU exited (status %d)", s.name, t.terminationStatus];
            [s setStateOnMain:QemuVmStateStopped];
        };
        NSError *launchErr = nil;
        if (![task launchAndReturnError:&launchErr]) {
            [self setStateOnMain:QemuVmStateStopped];
            done(launchErr ?: [self err:@"qemu launch failed"]);
            return;
        }
        self.task = task;
        [self logFmt:@"[%@] QEMU launched as root (pid %d)", self.name, task.processIdentifier];
    } else {
        NSError *elevErr = nil;
        if (![self launchElevated:qemu args:args error:&elevErr]) {
            [self setStateOnMain:QemuVmStateStopped];
            done(elevErr ?: [self err:@"elevated qemu launch failed"]);
            return;
        }
        [self logFmt:@"[%@] QEMU launched with elevated privileges (vmnet)", self.name];
        [self watchElevatedExit];
    }

    /* Map the published backing as the transport the channel helpers use. */
    self.transport = [[AsbIvshmemTransport alloc] initWithBackingPath:ivsPath];
    if (!self.transport) { [self logFmt:@"[%@] WARN: transport map failed", self.name]; }

    [self setStateOnMain:QemuVmStateRunning];
    done(nil);
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

/* Launch `tool` as root via AuthorizationExecuteWithPrivileges (pops the admin prompt). Stores the
 * AuthorizationRef + the child's stdout FILE* (whose EOF signals process exit). */
- (BOOL)launchElevated:(NSString *)tool args:(NSArray<NSString *> *)args error:(NSError **)err {
    AuthorizationRef auth = NULL;
    OSStatus s = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                     kAuthorizationFlagDefaults, &auth);
    if (s != errAuthorizationSuccess) { if (err) *err = [self err:@"AuthorizationCreate failed"]; return NO; }

    AuthorizationItem item = { kAuthorizationRightExecute, 0, NULL, 0 };
    AuthorizationRights rights = { 1, &item };
    AuthorizationFlags flags = kAuthorizationFlagDefaults | kAuthorizationFlagInteractionAllowed |
                               kAuthorizationFlagPreAuthorize | kAuthorizationFlagExtendRights;
    s = AuthorizationCopyRights(auth, &rights, kAuthorizationEmptyEnvironment, flags, NULL);
    if (s != errAuthorizationSuccess) {
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        if (err) *err = [self err:@"administrator authorization was denied (required for VM networking)"];
        return NO;
    }

    NSUInteger n = args.count;
    char **argv = calloc(n + 1, sizeof(char *));
    for (NSUInteger i = 0; i < n; i++) argv[i] = strdup(args[i].UTF8String);
    argv[n] = NULL;

    FILE *outPipe = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    s = AuthorizationExecuteWithPrivileges(auth, tool.UTF8String,
                                           kAuthorizationFlagDefaults, argv, &outPipe);
#pragma clang diagnostic pop
    for (NSUInteger i = 0; i < n; i++) free(argv[i]);
    free(argv);

    if (s != errAuthorizationSuccess) {
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        if (err) *err = [self err:@"failed to launch QEMU with elevated privileges"];
        return NO;
    }
    _authRef = auth;
    _elevatedPipe = outPipe;
    return YES;
}

/* The AEWP child isn't a waitpid-able child, so detect exit by draining its stdout to EOF. */
- (void)watchElevatedExit {
    __weak QemuVm *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        QemuVm *s = weakSelf;
        FILE *p = s ? s->_elevatedPipe : NULL;
        if (!p) return;
        char buf[1024];
        while (fread(buf, 1, sizeof buf, p) > 0) { /* drain; QEMU chatter goes to stderr */ }
        s = weakSelf;
        if (!s) { return; }
        fclose(p);
        s->_elevatedPipe = NULL;
        if (s->_authRef) { AuthorizationFree(s->_authRef, kAuthorizationFlagDefaults); s->_authRef = NULL; }
        [s.transport close];
        s.transport = nil;
        [s logFmt:@"[%@] QEMU (elevated) exited", s.name];
        [s setStateOnMain:QemuVmStateStopped];
    });
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
    [self logFmt:@"[%@] force stop (monitor quit)", self.name];
    if (![self sendMonitor:@"quit"]) {
        @try { [self.task terminate]; } @catch (__unused NSException *e) {}
    }
}

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
