#import "iso_patch_mac.h"
#import "vm_dir.h"               /* AppSandbox support root (sibling of VMs/) for the driver cache */
#import <Security/Security.h>
#import <Security/AuthorizationTags.h>
#import <Carbon/Carbon.h>
#import "../../tools/provision/win_provision.h"  /* shared answer-file generator (same source the Windows backend uses) */
#import "../../tools/provision/mac_account_hash.h"  /* shared macOS ShadowHash + kcpassword encoders */

/* AuthorizationExecuteWithPrivileges has been deprecated since 10.7 but is
 * still functional. Suppress the warning locally; we'll migrate to
 * SMAppService when ready. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static AuthorizationRef  g_auth = NULL;
static dispatch_source_t g_authKeepAlive = NULL;

static NSString *windows_input_locale(void) {
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (!source) return nil;
    NSString *sourceID = [(__bridge NSString *)TISGetInputSourceProperty(source,
                                                    kTISPropertyInputSourceID) copy];
    CFRelease(source);
    NSDictionary<NSString *, NSString *> *layouts = @{
        @"com.apple.keylayout.ABC": @"0409:00000409",
        @"com.apple.keylayout.US": @"0409:00000409",
        @"com.apple.keylayout.USInternational-PC": @"0409:00020409",
        @"com.apple.keylayout.Dvorak": @"0409:00010409",
        @"com.apple.keylayout.Dvorak-Left": @"0409:00030409",
        @"com.apple.keylayout.Dvorak-Right": @"0409:00040409",
        @"com.apple.keylayout.British": @"0809:00000809",
        @"com.apple.keylayout.British-PC": @"0809:00000809",
        @"com.apple.keylayout.German": @"0407:00000407",
        @"com.apple.keylayout.German-DIN-2137": @"0407:00000407",
        @"com.apple.keylayout.Austrian": @"0c07:00000407",
        @"com.apple.keylayout.SwissGerman": @"0807:00000807",
        @"com.apple.keylayout.French": @"040c:0000040c",
        @"com.apple.keylayout.French-PC": @"040c:0000040c",
        @"com.apple.keylayout.CanadianFrench-PC": @"0c0c:00001009",
        @"com.apple.keylayout.SwissFrench": @"100c:0000100c",
        @"com.apple.keylayout.Spanish-ISO": @"0c0a:0000040a",
        @"com.apple.keylayout.Italian-Pro": @"0410:00000410",
        @"com.apple.keylayout.Portuguese": @"0816:00000816",
        @"com.apple.keylayout.Brazilian-ABNT2": @"0416:00010416",
        @"com.apple.keylayout.Russian": @"0419:00000419",
        @"com.apple.keylayout.RussianWin": @"0419:00000419",
        @"com.apple.keylayout.PolishPro": @"0415:00000415",
        @"com.apple.inputmethod.SCIM.ITABC": @"zh-CN",
        @"com.apple.inputmethod.TCIM.Zhuyin": @"zh-TW",
        @"com.apple.inputmethod.TCIM.Cangjie": @"0404:{531FDEBF-9B4C-4A43-A2AA-960E8FCDC732}{4BDF9F03-C7D3-11D4-B2AB-0080C882687E}",
        @"com.apple.inputmethod.TCIM.Sucheng": @"0404:{531FDEBF-9B4C-4A43-A2AA-960E8FCDC732}{6024B45F-5C54-11D4-B921-0080C882687E}",
        @"com.apple.inputmethod.Korean.2SetKorean": @"ko-KR"
    };
    NSString *locale = sourceID ? layouts[sourceID] : nil;
    if (!locale && [sourceID hasPrefix:@"com.apple.inputmethod.Kotoeri."])
        locale = @"ja-JP";
    if (sourceID && !locale)
        NSLog(@"Windows keyboard: no input profile mapping for %@", sourceID);
    return locale;
}

/* Keep-alive interval: shorter than the default 300s TTL of
 * kAuthorizationRightExecute so the right never expires while the app
 * runs. Without this, a long install (>5 min) causes AEWP to prompt a
 * second time when stage finally runs. */
#define AUTH_KEEPALIVE_SECONDS 240

@implementation IsoPatchMac

+ (nullable NSString *)toolPath {
    NSFileManager *fm = [NSFileManager defaultManager];

    /* 1. Bundled next to the host app. */
    NSString *res = [[NSBundle mainBundle].resourcePath
                        stringByAppendingPathComponent:@"iso-patch-mac"];
    if (res && [fm fileExistsAtPath:res]) return res;

    /* 2. Dev fallback: walk up from the bundle to the source tree. */
    NSString *cur = [NSBundle mainBundle].bundlePath;
    for (int i = 0; i < 6 && cur.length > 1; i++) {
        NSString *candidate = [cur stringByAppendingPathComponent:
                                 @"tools/iso-patch-mac/build/iso-patch-mac"];
        if ([fm fileExistsAtPath:candidate]) return candidate;
        cur = [cur stringByDeletingLastPathComponent];
    }
    return nil;
}

#pragma mark - Authorization (for stage)

+ (void)startKeepAlive {
    if (g_authKeepAlive) return;
    g_authKeepAlive = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(g_authKeepAlive,
        dispatch_time(DISPATCH_TIME_NOW, AUTH_KEEPALIVE_SECONDS * NSEC_PER_SEC),
        AUTH_KEEPALIVE_SECONDS * NSEC_PER_SEC,
        10 * NSEC_PER_SEC);
    dispatch_source_set_event_handler(g_authKeepAlive, ^{
        if (!g_auth) return;
        /* Non-interactive refresh: if rights are still valid, this silently
         * resets their TTL. If they've somehow expired, we fail silently
         * here and the next real call to ensureAuthorization will prompt. */
        AuthorizationItem right = { kAuthorizationRightExecute, 0, NULL, 0 };
        AuthorizationRights rights = { 1, &right };
        AuthorizationFlags flags = kAuthorizationFlagDefaults
                                 | kAuthorizationFlagExtendRights;
        (void)AuthorizationCopyRights(g_auth, &rights, NULL, flags, NULL);
    });
    /* Free g_auth from the cancel handler: GCD runs it only after any in-flight
     * event handler returns. */
    dispatch_source_set_cancel_handler(g_authKeepAlive, ^{
        if (g_auth) {
            AuthorizationFree(g_auth, kAuthorizationFlagDestroyRights);
            g_auth = NULL;
        }
    });
    dispatch_resume(g_authKeepAlive);
}

+ (BOOL)ensureAuthorization:(NSError **)errOut {
    /* Allocate the ref on first call. */
    if (!g_auth) {
        OSStatus s = AuthorizationCreate(NULL, NULL,
                                          kAuthorizationFlagDefaults, &g_auth);
        if (s != errAuthorizationSuccess) {
            g_auth = NULL;
            if (errOut) *errOut = [NSError errorWithDomain:NSOSStatusErrorDomain code:s
                                                  userInfo:@{NSLocalizedDescriptionKey:
                                                             @"AuthorizationCreate failed"}];
            return NO;
        }
    }

    /* Always re-check rights. If they're still valid (keep-alive kept them
     * fresh), this is silent. If they've expired, this is where the UI
     * prompt fires — which should be at VM-creation time, not mid-install. */
    AuthorizationItem right = { kAuthorizationRightExecute, 0, NULL, 0 };
    AuthorizationRights rights = { 1, &right };
    AuthorizationFlags flags = kAuthorizationFlagDefaults
                             | kAuthorizationFlagInteractionAllowed
                             | kAuthorizationFlagPreAuthorize
                             | kAuthorizationFlagExtendRights;
    OSStatus s = AuthorizationCopyRights(g_auth, &rights, NULL, flags, NULL);
    if (s != errAuthorizationSuccess) {
        if (errOut) *errOut = [NSError errorWithDomain:NSOSStatusErrorDomain code:s
                                              userInfo:@{NSLocalizedDescriptionKey:
                                                         @"User denied admin prompt"}];
        return NO;
    }

    [self startKeepAlive];
    return YES;
}

+ (void)releaseAuthorization {
    if (g_authKeepAlive) {
        /* The cancel handler frees g_auth; don't free it here as well. */
        dispatch_source_cancel(g_authKeepAlive);
        g_authKeepAlive = NULL;
        return;
    }
    /* Keep-alive was never started (auth created but rights check failed, so
     * no timer/handler ever existed); free directly. */
    if (g_auth) {
        AuthorizationFree(g_auth, kAuthorizationFlagDestroyRights);
        g_auth = NULL;
    }
}

+ (BOOL)preauthorize:(NSError **)error {
    /* Already root (the headless daemon runs under sudo): no AuthorizationRef
     * needed -- runPrivilegedArgs launches the stage step directly. Keeps
     * headless fully non-interactive (no username/password prompt). */
    if (geteuid() == 0) return YES;
    return [self ensureAuthorization:error];
}

#pragma mark - Stdout protocol parser

/* Parses a single line from the CLI's stdout; updates progress/final state. */
+ (void)parseLine:(NSString *)line
         progress:(IsoPatchProgress)progressBlock
        finalPath:(NSString **)finalPath
        finalErr:(NSString **)finalErr {
    if ([line hasPrefix:@"STATUS:"]) {
        NSString *s = [line substringFromIndex:7];
        if (progressBlock) {
            dispatch_async(dispatch_get_main_queue(), ^{ progressBlock(-1.0, s); });
        }
        return;
    }
    if ([line hasPrefix:@"PROGRESS:"]) {
        NSString *rest = [line substringFromIndex:9];
        NSRange colon = [rest rangeOfString:@":"];
        int pct = (int)[rest intValue];
        NSString *step = (colon.location != NSNotFound)
            ? [rest substringFromIndex:colon.location + 1] : @"";
        double frac = (double)pct / 100.0;
        if (progressBlock) {
            dispatch_async(dispatch_get_main_queue(), ^{ progressBlock(frac, step); });
        }
        return;
    }
    if ([line hasPrefix:@"LOG:"]) {
        NSLog(@"iso-patch-mac: %@", [line substringFromIndex:4]);
        return;
    }
    if ([line hasPrefix:@"LANG:"]) {
        /* Detected ISO language -> delivered raw via the LANG sentinel. buildWindowsDiskWithISO
         * re-generates unattend.xml in-process with this tag (mirrors the Windows backend's
         * asb_core.c LANG: re-gen) and re-emits the "Detected ISO language" log to the caller. */
        NSString *tag = [line substringFromIndex:5];
        if (progressBlock) {
            dispatch_async(dispatch_get_main_queue(), ^{ progressBlock(ISO_PATCH_PROGRESS_LANG, tag); });
        }
        return;
    }
    if ([line hasPrefix:@"DONE:"]) {
        if (finalPath) *finalPath = [line substringFromIndex:5];
        return;
    }
    if ([line hasPrefix:@"ERROR:"]) {
        if (finalErr) *finalErr = [line substringFromIndex:6];
        return;
    }
}

/* Consumes the pipe, calling parseLine: for each complete line. Fires
 * `completion` on the main queue when EOF is reached. */
+ (void)drainFileHandle:(NSFileHandle *)fh
                 pipeFP:(FILE *)pipeFP
                progress:(IsoPatchProgress)progressBlock
              completion:(IsoPatchCompletion)completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        __block NSString *finalPath = nil;
        __block NSString *finalErr  = nil;

        NSMutableData *buf = [NSMutableData data];
        while (true) {
            NSData *chunk = nil;
            if (fh) {
                chunk = [fh availableData];
                if (chunk.length == 0) break;
            } else if (pipeFP) {
                char cbuf[4096];
                size_t r = fread(cbuf, 1, sizeof(cbuf), pipeFP);
                if (r == 0) break;
                chunk = [NSData dataWithBytes:cbuf length:r];
            } else {
                break;
            }
            [buf appendData:chunk];

            const char *bytes = buf.bytes;
            NSUInteger len = buf.length;
            NSUInteger start = 0;
            for (NSUInteger i = 0; i < len; i++) {
                if (bytes[i] != '\n') continue;
                NSString *line = [[NSString alloc] initWithBytes:bytes + start
                                                           length:i - start
                                                         encoding:NSUTF8StringEncoding];
                if (line.length) {
                    [self parseLine:line
                           progress:progressBlock
                          finalPath:&finalPath
                          finalErr:&finalErr];
                }
                start = i + 1;
            }
            if (start > 0) {
                [buf replaceBytesInRange:NSMakeRange(0, start) withBytes:NULL length:0];
            }
        }
        if (pipeFP) fclose(pipeFP);

        NSError *err = nil;
        if (finalErr) {
            err = [NSError errorWithDomain:@"IsoPatchMac" code:2
                     userInfo:@{NSLocalizedDescriptionKey: finalErr}];
        } else if (!finalPath) {
            err = [NSError errorWithDomain:@"IsoPatchMac" code:3
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"iso-patch-mac exited without DONE or ERROR"}];
        }
        dispatch_async(dispatch_get_main_queue(), ^{ completion(err); });
    });
}

#pragma mark - Unprivileged (NSTask)

/* Returns the launched task, or nil on failure (completion fired with error). */
+ (nullable NSTask *)runUnprivilegedArgs:(NSArray<NSString *> *)args
                                progress:(IsoPatchProgress)progressBlock
                              completion:(IsoPatchCompletion)completion {
    NSString *tool = [self toolPath];
    if (!tool) {
        completion([NSError errorWithDomain:@"IsoPatchMac" code:1
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"iso-patch-mac binary not found"}]);
        return nil;
    }

    NSTask *task = [[NSTask alloc] init];
    task.launchPath = tool;
    task.arguments  = args;
    NSPipe *outPipe = [NSPipe pipe];
    task.standardOutput = outPipe;
    task.standardError  = outPipe; /* merge so errors show up in our parser */

    NSError *launchErr = nil;
    if (![task launchAndReturnError:&launchErr]) {
        completion(launchErr);
        return nil;
    }

    [self drainFileHandle:outPipe.fileHandleForReading
                   pipeFP:NULL
                 progress:progressBlock
               completion:completion];
    return task;
}

#pragma mark - Privileged (AEWP)

+ (void)runPrivilegedArgs:(NSArray<NSString *> *)args
                  progress:(IsoPatchProgress)progressBlock
                completion:(IsoPatchCompletion)completion {
    NSString *tool = [self toolPath];
    if (!tool) {
        completion([NSError errorWithDomain:@"IsoPatchMac" code:1
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"iso-patch-mac binary not found"}]);
        return;
    }

    /* Already root (headless daemon under sudo): a plain NSTask child IS
     * privileged -- skip AuthorizationExecuteWithPrivileges entirely, so no
     * interactive prompt can ever appear in headless mode. */
    if (geteuid() == 0) {
        [self runUnprivilegedArgs:args progress:progressBlock completion:completion];
        return;
    }

    NSError *authErr = nil;
    if (![self ensureAuthorization:&authErr]) {
        completion(authErr);
        return;
    }

    NSUInteger n = args.count;
    char **cargs = calloc(n + 1, sizeof(char *));
    for (NSUInteger i = 0; i < n; i++) cargs[i] = strdup([args[i] UTF8String]);
    cargs[n] = NULL;

    FILE *pipe = NULL;
    OSStatus s = AuthorizationExecuteWithPrivileges(g_auth,
        tool.UTF8String,
        kAuthorizationFlagDefaults,
        cargs,
        &pipe);

    for (NSUInteger i = 0; i < n; i++) free(cargs[i]);
    free(cargs);

    if (s != errAuthorizationSuccess) {
        completion([NSError errorWithDomain:NSOSStatusErrorDomain code:s
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"AuthorizationExecuteWithPrivileges failed"}]);
        return;
    }

    [self drainFileHandle:nil
                   pipeFP:pipe
                 progress:progressBlock
               completion:completion];
}

#pragma mark - Public API

/* Keyed by VM name. Mutated only on s_fetchQueue. */
static NSMutableDictionary<NSString *, NSTask *> *s_fetchTasks = nil;
static dispatch_queue_t s_fetchQueue = NULL;

static void ensure_fetch_registry(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        s_fetchTasks = [NSMutableDictionary dictionary];
        s_fetchQueue = dispatch_queue_create("com.appsandbox.iso-patch.fetch",
                                             DISPATCH_QUEUE_SERIAL);
    });
}

+ (void)fetchLatestIpswToURL:(NSURL *)ipswURL
                        forVm:(NSString *)vmName
                     progress:(IsoPatchProgress)progressBlock
                   completion:(IsoPatchCompletion)completion {
    ensure_fetch_registry();

    NSString *key = [vmName copy];
    IsoPatchCompletion wrapped = ^(NSError *err) {
        dispatch_async(s_fetchQueue, ^{ [s_fetchTasks removeObjectForKey:key]; });
        if (completion) completion(err);
    };

    NSTask *task = [self runUnprivilegedArgs:@[@"fetch-ipsw", @"--output", ipswURL.path]
                                     progress:progressBlock
                                   completion:wrapped];
    if (task) {
        dispatch_async(s_fetchQueue, ^{ s_fetchTasks[key] = task; });
    }
}

+ (void)cancelFetchForVm:(NSString *)vmName {
    ensure_fetch_registry();
    NSString *key = [vmName copy];
    dispatch_async(s_fetchQueue, ^{
        NSTask *task = s_fetchTasks[key];
        if (!task) return;
        [s_fetchTasks removeObjectForKey:key];
        if (task.isRunning) {
            @try { [task terminate]; }
            @catch (NSException *e) { (void)e; }
        }
    });
}

+ (void)installMacOSWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                    ipswURL:(NSURL *)ipswURL
                       ramMb:(int)ramMb
                       cpus:(int)cpus
                      diskGb:(int)diskGb
                   progress:(IsoPatchProgress)progressBlock
                 completion:(IsoPatchCompletion)completion {
    NSArray *args = @[
        @"install",
        @"--name",     name,
        @"--vm-dir",   vmDir.path,
        @"--disk-path", [VmDir diskImageURLFor:name].path,
        @"--ipsw",     ipswURL.path,
        @"--ram-mb",   [NSString stringWithFormat:@"%d", ramMb],
        @"--cpus",     [NSString stringWithFormat:@"%d", cpus],
        @"--disk-gb",  [NSString stringWithFormat:@"%d", diskGb],
    ];
    [self runUnprivilegedArgs:args
                     progress:progressBlock
                   completion:completion];
}

#pragma mark - build-windows (unprivileged)

/* Generate unattend.xml in-process -- the password is encoded HERE (in the daemon) and only the
 * obfuscated answer file is ever handed to the build-windows child, which never sees the plaintext.
 * This is the exact mirror of generate_unattend_vhdx -> asb_provision_unattend in the Windows
 * backend (src/backend_win/disk_util.c / asb_core.c vhdx_create_thread). */
static BOOL write_unattend_xml(NSString *path, NSString *vmName, NSString *user,
                               NSString *pass, NSString *lang, NSString *inputLocale, BOOL testMode) {
    FILE *fu = fopen(path.fileSystemRepresentation, "wb");
    if (!fu) return NO;
    int rc = asb_provision_unattend(fu, vmName.UTF8String, user.UTF8String,
                                    pass.length ? pass.UTF8String : "",
                                    "arm64", testMode ? 1 : 0, /*is_arm64=*/1,
                                    lang.length ? lang.UTF8String : "en-US",
                                    inputLocale.UTF8String);
    fclose(fu);
    return rc == 0;
}

/* Generate setup.cmd + SetupComplete.cmd in-process (no secrets; same shared source as Windows). */
static BOOL write_prov_scripts(NSString *dir, NSString *sshMsiName) {
    BOOL ok = YES;
    FILE *fs = fopen([dir stringByAppendingPathComponent:@"setup.cmd"].fileSystemRepresentation, "wb");
    if (fs) { if (asb_provision_setup_cmd(fs) != 0) ok = NO; fclose(fs); } else ok = NO;
    FILE *fc = fopen([dir stringByAppendingPathComponent:@"SetupComplete.cmd"].fileSystemRepresentation, "wb");
    if (fc) { if (asb_provision_setupcomplete(fc, sshMsiName.length ? sshMsiName.UTF8String : NULL) != 0) ok = NO; fclose(fc); } else ok = NO;
    return ok;
}

+ (void)buildWindowsDiskWithISO:(NSURL *)isoURL
                        outDisk:(NSURL *)outDiskURL
               signedPayloadZip:(NSString *)signedPayloadZip
                      netkvmZip:(nullable NSString *)netkvmZip
                     sshMsiPath:(nullable NSString *)sshMsiPath
                         vmName:(NSString *)vmName
                      adminUser:(NSString *)adminUser
                      adminPass:(NSString *)adminPass
                           lang:(nullable NSString *)lang
                         diskGb:(int)diskGb
                       testMode:(BOOL)testMode
                       progress:(nullable IsoPatchProgress)progressBlock
                     completion:(IsoPatchCompletion)completion {
    NSFileManager *fm = [NSFileManager defaultManager];

    /* Generate the provisioning files IN-PROCESS (mirrors the Windows daemon's vhdx_create_thread:
     * it generates unattend.xml/setup.cmd/SetupComplete.cmd itself and hands iso-patch.exe only a
     * --stage manifest). The password is encoded here via the shared asb_provision_unattend and the
     * child receives only --prov-dir -- the plaintext never crosses the process boundary. The initial
     * unattend uses en-US; build-windows reports the install.wim language via the LANG sentinel and we
     * re-generate it below, exactly like asb_core.c re-runs generate_unattend_vhdx on "LANG:". */
    NSString *provDir = [NSTemporaryDirectory() stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"asb-winprov-%u", arc4random()]];
    [fm removeItemAtPath:provDir error:nil];
    [fm createDirectoryAtPath:provDir withIntermediateDirectories:YES
                   attributes:@{NSFilePosixPermissions: @(0700)} error:nil];

    NSString *sshMsiName = (sshMsiPath.length && [fm fileExistsAtPath:sshMsiPath])
                              ? sshMsiPath.lastPathComponent : nil;
    BOOL hasOverride = (lang.length > 0);
    NSString *initialLang = hasOverride ? lang : @"en-US";
    NSString *inputLocale = windows_input_locale();
    if (!write_unattend_xml([provDir stringByAppendingPathComponent:@"unattend.xml"],
                            vmName, adminUser, adminPass, initialLang, inputLocale, testMode) ||
        !write_prov_scripts(provDir, sshMsiName)) {
        [fm removeItemAtPath:provDir error:nil];
        completion([NSError errorWithDomain:@"IsoPatchMac" code:7
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"failed to generate provisioning files"}]);
        return;
    }

    NSMutableArray *args = [@[
        @"build-windows",
        @"--iso",      isoURL.path,
        @"--out",      outDiskURL.path,
        @"--signed-payload-zip", signedPayloadZip,
        @"--prov-dir", provDir,                     /* daemon-generated answer file + scripts (no password on argv) */
        @"--disk-gb",  [NSString stringWithFormat:@"%d", diskGb > 0 ? diskGb : 64],
        @"--test-mode", (testMode ? @"1" : @"0"),   /* honor the create-time choice (mirrors Windows) */
    ] mutableCopy];
    /* Pass --lang ONLY for an explicit override; without it build-windows auto-detects the ISO's
     * language and reports it via LANG: so we re-generate unattend.xml in-process below. */
    if (hasOverride)       { [args addObject:@"--lang"];       [args addObject:lang]; }
    if (netkvmZip.length)  { [args addObject:@"--netkvm-zip"]; [args addObject:netkvmZip]; }
    if (sshMsiPath.length) { [args addObject:@"--ssh-msi"];    [args addObject:sshMsiPath]; }

    /* Wrap progress: intercept the LANG sentinel to re-generate unattend.xml in-process with the
     * detected language (the staging file build-windows copies in is overwritten before it stages,
     * just like the Windows backend overwrites _vhdx_staging\unattend.xml on "LANG:"). */
    IsoPatchProgress wrapped = ^(double frac, NSString *step) {
        if (frac == ISO_PATCH_PROGRESS_LANG) {
            if (!hasOverride) {
                (void)write_unattend_xml([provDir stringByAppendingPathComponent:@"unattend.xml"],
                                         vmName, adminUser, adminPass, step, inputLocale, testMode);
            }
            if (progressBlock)
                progressBlock(ISO_PATCH_PROGRESS_LOG,
                              [@"Detected ISO language: " stringByAppendingString:step]);
            return;
        }
        if (progressBlock) progressBlock(frac, step);
    };

    [self runUnprivilegedArgs:args
                     progress:wrapped
                   completion:^(NSError * _Nullable err) {
        /* unattend.xml holds the obfuscated password; drop the staging dir once the build is done. */
        [[NSFileManager defaultManager] removeItemAtPath:provDir error:nil];
        completion(err);
    }];
}

/* Mirror of ensure_ssh_msi_cached: download the OpenSSH ARM64 MSI once, cache it under
 * <Caches>/AppSandbox/, return the cached path (or nil). Blocking; the build flow runs off-main. */
+ (NSString *)ensureOpenSSHMsiCached {
    NSString *name = @"OpenSSH-ARM64-v10.0.0.0.msi";
    NSString *urlStr = @"https://github.com/PowerShell/Win32-OpenSSH/releases/download/"
                        "10.0.0.0p2-Preview/OpenSSH-ARM64-v10.0.0.0.msi";
    /* Cache under the AppSandbox support dir (sibling of VMs/ + restore.ipsw), NOT ~/Library/Caches —
     * VmDir's root is sudo-aware, so the daemon (root) and the GUI (user) share one cache. */
    NSString *cacheDir = [[VmDir vmsRootDirectory] URLByDeletingLastPathComponent].path;
    [[NSFileManager defaultManager] createDirectoryAtPath:cacheDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [cacheDir stringByAppendingPathComponent:name];
    if ([[NSFileManager defaultManager] fileExistsAtPath:dst]) return dst;

    /* Synchronous download (follows the GitHub->CDN redirect). */
    __block NSData *data = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *t = [[NSURLSession sharedSession]
        dataTaskWithURL:[NSURL URLWithString:urlStr]
      completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
        NSInteger code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)resp).statusCode : 0;
        if (d.length && (code == 200 || code == 0) && !err) data = d;
        dispatch_semaphore_signal(sem);
    }];
    [t resume];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)180 * NSEC_PER_SEC)) != 0)
        [t cancel];   /* timeout: don't leak the in-flight task */
    if (data.length && [data writeToFile:dst atomically:YES]) return dst;
    return nil;
}

/* See iso_patch_mac.h. Cache the signed Windows release zip. Mirrors ensureOpenSSHMsiCached: a single
 * atomic write to a version-named file under the AppSandbox support dir (sibling of VMs/), shared by
 * the root daemon + GUI. build-windows extracts the needed members into its own per-build temp. */
+ (nullable NSString *)ensureSignedWinPayloadZipCached {
    /* Signed ARM64 release: EV-signed agent EXEs + MS-attestation-signed drivers (VDD/VAD/AppSandboxSHM/
     * devcon). The release version is pinned in the cached filename, so a newer release auto-invalidates
     * the cache. The URL must stay publicly fetchable by an unauthenticated client. */
    NSString *name   = @"AppSandbox-0.1.5-win-arm64.zip";
    NSString *urlStr = @"https://github.com/jamesstringer90/appsandbox/releases/download/v0.1.5/AppSandbox-0.1.5-win-arm64.zip";
    NSString *cacheDir = [[VmDir vmsRootDirectory] URLByDeletingLastPathComponent].path;
    [[NSFileManager defaultManager] createDirectoryAtPath:cacheDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [cacheDir stringByAppendingPathComponent:name];
    if ([[NSFileManager defaultManager] fileExistsAtPath:dst]) return dst;   /* cache hit */

    /* Synchronous download (follows the GitHub->CDN redirect). The atomic writeToFile makes concurrent
     * creates race-safe (worst case: a redundant download), exactly like the OpenSSH MSI. */
    __block NSData *data = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *t = [[NSURLSession sharedSession]
        dataTaskWithURL:[NSURL URLWithString:urlStr]
      completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
        NSInteger code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)resp).statusCode : 0;
        if (d.length && (code == 200 || code == 0) && !err) data = d;
        dispatch_semaphore_signal(sem);
    }];
    [t resume];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)300 * NSEC_PER_SEC)) != 0)
        [t cancel];   /* timeout: don't leak the in-flight task */
    if (data.length && [data writeToFile:dst atomically:YES]) return dst;
    return nil;
}

/* See iso_patch_mac.h. Cache the vendored NetKVM zip. Mirrors ensureSignedWinPayloadZipCached: a single
 * atomic write to a fixed-named file under the AppSandbox support dir (sibling of VMs/), shared by the
 * root daemon + GUI. Pulled from the public repo (raw.githubusercontent.com); the file never changes. */
+ (nullable NSString *)ensureNetkvmZipCached {
    NSString *name   = @"netkvm-arm64.zip";
    NSString *urlStr = @"https://raw.githubusercontent.com/jamesstringer90/appsandbox/win-on-mac/vendor/virtio-win/netkvm-arm64.zip";
    NSString *cacheDir = [[VmDir vmsRootDirectory] URLByDeletingLastPathComponent].path;
    [[NSFileManager defaultManager] createDirectoryAtPath:cacheDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [cacheDir stringByAppendingPathComponent:name];
    if ([[NSFileManager defaultManager] fileExistsAtPath:dst]) return dst;   /* cache hit */

    __block NSData *data = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *t = [[NSURLSession sharedSession]
        dataTaskWithURL:[NSURL URLWithString:urlStr]
      completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
        NSInteger code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)resp).statusCode : 0;
        if (d.length && (code == 200 || code == 0) && !err) data = d;
        dispatch_semaphore_signal(sem);
    }];
    [t resume];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)180 * NSEC_PER_SEC)) != 0)
        [t cancel];
    if (data.length && [data writeToFile:dst atomically:YES]) return dst;
    return nil;
}

#pragma mark - stage (privileged)

+ (NSString *)formatManifestForAgentDir:(NSString *)dir
                              binarySrc:(NSString *)binPath {
    /* Columns: src <TAB> dest_rel <TAB> mode_octal <TAB> owner
     * Clipboard binary + plist are staged into /Library/AppSandbox/;
     * firstboot.sh moves the plist into /Library/LaunchAgents/ on first
     * boot (same root-chown dance as the agent LaunchDaemon). */
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *clipBin = [dir stringByAppendingPathComponent:@"appsandbox-clipboard"];
    if (![fm fileExistsAtPath:clipBin])
        clipBin = [dir stringByAppendingPathComponent:@"build/appsandbox-clipboard"];
    BOOL haveClip      = [fm fileExistsAtPath:clipBin];
    NSString *clipPlist = [dir stringByAppendingPathComponent:@"com.appsandbox.clipboard.plist"];
    BOOL haveClipPlist = [fm fileExistsAtPath:clipPlist];

    NSMutableString *m = [NSMutableString stringWithFormat:
        @"%@\tLibrary/AppSandbox/appsandbox-agent\t0755\troot:wheel\n"
        @"%@/com.appsandbox.agent.plist\tLibrary/AppSandbox/com.appsandbox.agent.plist\t0644\troot:wheel\n"
        @"%@/firstboot.sh\tLibrary/AppSandbox/firstboot.sh\t0755\troot:wheel\n"
        @"%@/com.appsandbox.firstboot.plist\tLibrary/LaunchDaemons/com.appsandbox.firstboot.plist\t0644\troot:wheel\n",
        binPath, dir, dir, dir];
    if (haveClip) {
        [m appendFormat:@"%@\tLibrary/AppSandbox/appsandbox-clipboard\t0755\troot:wheel\n",
            clipBin];
    }
    if (haveClipPlist) {
        [m appendFormat:@"%@\tLibrary/AppSandbox/com.appsandbox.clipboard.plist\t0644\troot:wheel\n",
            clipPlist];
    }
    return m;
}

+ (void)stageAgentIntoDiskAtURL:(NSURL *)diskURL
               agentResourceDir:(NSString *)agentResDir
                       adminUser:(NSString *)adminUser
                       adminPass:(NSString *)adminPass
                    computerName:(NSString *)computerName
                     sshEnabled:(BOOL)sshEnabled
                       progress:(IsoPatchProgress)progressBlock
                     completion:(IsoPatchCompletion)completion {
    NSFileManager *fm = [NSFileManager defaultManager];

    NSString *binPath = [agentResDir stringByAppendingPathComponent:@"appsandbox-agent"];
    if (![fm fileExistsAtPath:binPath]) {
        binPath = [agentResDir stringByAppendingPathComponent:@"build/appsandbox-agent"];
    }
    if (![fm fileExistsAtPath:binPath]) {
        completion([NSError errorWithDomain:@"IsoPatchMac" code:4
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"appsandbox-agent binary not found"}]);
        return;
    }

    for (NSString *name in @[@"com.appsandbox.agent.plist",
                             @"com.appsandbox.firstboot.plist",
                             @"firstboot.sh"]) {
        if (![fm fileExistsAtPath:[agentResDir stringByAppendingPathComponent:name]]) {
            completion([NSError errorWithDomain:@"IsoPatchMac" code:5
                         userInfo:@{NSLocalizedDescriptionKey:
                                    [NSString stringWithFormat:@"missing %@", name]}]);
            return;
        }
    }

    /* Write the manifest (0600). The account password is NEVER written to disk or put on argv: we
     * compute the macOS ShadowHash + kcpassword IN-PROCESS (mirrors the Windows path, where the
     * password is encoded in the daemon, not handed to the disk-applier) and pass only those derived,
     * non-plaintext blobs to the privileged stage child via 0600 files. Files are deleted on exit. */
    NSString *manifest = [self formatManifestForAgentDir:agentResDir binarySrc:binPath];
    NSString *manifestPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
                                [NSString stringWithFormat:@"iso-patch-mac-%u.tsv", arc4random()]];

    NSError *wErr = nil;
    if (![manifest writeToFile:manifestPath atomically:YES
                      encoding:NSUTF8StringEncoding error:&wErr]) {
        completion(wErr); return;
    }
    [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions: @(0600)}
                                     ofItemAtPath:manifestPath error:nil];

    /* Derive the credential blobs in-process (only when a password was set, matching the prior
     * behaviour where an empty password created no account). The plaintext stays in this process. */
    NSString *shadowhashPath = nil, *kcpasswordPath = nil;
    if (adminPass.length) {
        const char *pw = adminPass.UTF8String;
        size_t pwLen = strlen(pw);
        NSData *shd = asb_macos_shadow_hash_data(pw, pwLen);
        NSData *kc  = asb_macos_kcpassword(pw, pwLen);
        if (!shd || !kc) {
            [fm removeItemAtPath:manifestPath error:nil];
            completion([NSError errorWithDomain:@"IsoPatchMac" code:6
                         userInfo:@{NSLocalizedDescriptionKey:@"failed to derive account credential"}]);
            return;
        }
        shadowhashPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"iso-patch-mac-%u.shd", arc4random()]];
        kcpasswordPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"iso-patch-mac-%u.kc", arc4random()]];
        if (![shd writeToFile:shadowhashPath atomically:YES] ||
            ![kc  writeToFile:kcpasswordPath atomically:YES]) {
            [fm removeItemAtPath:manifestPath error:nil];
            [fm removeItemAtPath:shadowhashPath error:nil];
            [fm removeItemAtPath:kcpasswordPath error:nil];
            completion([NSError errorWithDomain:@"IsoPatchMac" code:6
                         userInfo:@{NSLocalizedDescriptionKey:@"failed to write credential tempfiles"}]);
            return;
        }
        for (NSString *p in @[shadowhashPath, kcpasswordPath])
            [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions: @(0600)}
                                             ofItemAtPath:p error:nil];
    }

    NSMutableArray *args = [@[
        @"stage",
        @"--disk", diskURL.path,
        @"--manifest", manifestPath,
        @"--user-shortname", adminUser,
        @"--user-realname", adminUser,
        @"--user-uid", @"501",
        @"--skip-setup-assistant",
        @"--auto-login",
    ] mutableCopy];
    if (shadowhashPath) { [args addObject:@"--shadowhash-file"]; [args addObject:shadowhashPath]; }
    if (kcpasswordPath) { [args addObject:@"--kcpassword-file"]; [args addObject:kcpasswordPath]; }
    if (sshEnabled) [args addObject:@"--enable-ssh"];
    if (computerName.length) {
        [args addObject:@"--computer-name"];
        [args addObject:computerName];
    }

    [self runPrivilegedArgs:args
                   progress:progressBlock
                 completion:^(NSError * _Nullable err) {
        /* Remove tempfiles ASAP (the credential blobs are derived, not plaintext). */
        [[NSFileManager defaultManager] removeItemAtPath:manifestPath error:nil];
        if (shadowhashPath) [[NSFileManager defaultManager] removeItemAtPath:shadowhashPath error:nil];
        if (kcpasswordPath) [[NSFileManager defaultManager] removeItemAtPath:kcpasswordPath error:nil];
        completion(err);
    }];
}

@end

#pragma clang diagnostic pop
