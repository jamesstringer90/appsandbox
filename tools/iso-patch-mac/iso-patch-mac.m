
#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/xattr.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <crt_externs.h>
#include <stdio.h>
#include <string.h>
#include "engine/win_disk.h"               /* from-scratch Windows-disk builder (apply WIM + stage) */
#include "engine/wim.h"                    /* WIM XML reader -> install image's default UI language */
#include "../provision/win_provision.h"    /* shared unattend/setup.cmd/SetupComplete.cmd generators */

/* ---- stdout protocol helpers ---- */

static volatile sig_atomic_t g_cancelled;
static pthread_mutex_t g_cancel_exit_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local BOOL g_cleaning_up;

static void cancel_on_signal(int sig) {
    (void)sig;
    __atomic_store_n(&g_cancelled, 1, __ATOMIC_RELAXED);
}

static void check_cancelled(void) {
    if (g_cleaning_up || !__atomic_load_n(&g_cancelled, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&g_cancel_exit_lock);
    g_cleaning_up = YES;
    fprintf(stdout, "ERROR:VM creation cancelled\n");
    fflush(stdout);
    exit(130);
}

static void emit(NSString *prefix, NSString *msg) {
    check_cancelled();
    fprintf(stdout, "%s:%s\n", prefix.UTF8String, msg.UTF8String);
    fflush(stdout);
}
static void emit_status(NSString *msg)   { emit(@"STATUS",   msg); }
static void emit_progress(int pct, NSString *step) {
    check_cancelled();
    fprintf(stdout, "PROGRESS:%d:%s\n", pct, step.UTF8String);
    fflush(stdout);
}
static void emit_log(NSString *msg)      { emit(@"LOG",      msg); }
static void emit_lang(NSString *tag)     { emit(@"LANG",     tag); }   /* detected ISO language (mirrors Windows iso-patch.c) */
static void emit_done(NSString *path)    { emit(@"DONE",     path); }
static void emit_error(NSString *msg)    { emit(@"ERROR",    msg); }

/* ---- Subprocess runner ---- */

static int run_tool(NSString *launchPath, NSArray<NSString *> *args,
                    NSString **stdoutStr, NSString **stderrStr) {
    check_cancelled();
    char **argv = calloc(args.count + 2, sizeof(char *));
    if (!argv) {
        if (stderrStr) *stderrStr = @"Cannot allocate tool arguments";
        return -1;
    }
    argv[0] = (char *)launchPath.fileSystemRepresentation;
    for (NSUInteger i = 0; i < args.count; ++i) argv[i + 1] = (char *)args[i].UTF8String;
    NSPipe *outPipe = [NSPipe pipe];
    NSPipe *errPipe = [NSPipe pipe];
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        free(argv);
        if (stderrStr) *stderrStr = @(strerror(rc));
        return -1;
    }
    posix_spawnattr_t attributes;
    rc = posix_spawnattr_init(&attributes);
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        free(argv);
        if (stderrStr) *stderrStr = @(strerror(rc));
        return -1;
    }
    rc = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    if (!rc) rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, outPipe.fileHandleForWriting.fileDescriptor, STDOUT_FILENO);
    if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, errPipe.fileHandleForWriting.fileDescriptor, STDERR_FILENO);
    for (NSFileHandle *handle in @[outPipe.fileHandleForReading, outPipe.fileHandleForWriting,
                                   errPipe.fileHandleForReading, errPipe.fileHandleForWriting]) {
        if (!rc) rc = posix_spawn_file_actions_addclose(&actions, handle.fileDescriptor);
    }
    pid_t pid = -1;
    if (!rc) rc = posix_spawn(&pid, argv[0], &actions, &attributes, argv, *_NSGetEnviron());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    free(argv);
    [outPipe.fileHandleForWriting closeFile];
    [errPipe.fileHandleForWriting closeFile];
    if (rc != 0) {
        if (stderrStr) *stderrStr = @(strerror(rc));
        return -1;
    }
    __block NSData *errD = nil;
    dispatch_group_t readers = dispatch_group_create();
    dispatch_group_async(readers, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        errD = [errPipe.fileHandleForReading readDataToEndOfFile];
    });
    NSData *outD = [outPipe.fileHandleForReading readDataToEndOfFile];
    dispatch_group_wait(readers, DISPATCH_TIME_FOREVER);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (stdoutStr)
        *stdoutStr = [[NSString alloc] initWithData:outD encoding:NSUTF8StringEncoding];
    if (stderrStr)
        *stderrStr = [[NSString alloc] initWithData:errD encoding:NSUTF8StringEncoding];
    return waited < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* ---- Mount lifecycle ---- */

typedef struct {
    NSString *diskDev;   /* e.g. /dev/disk7     — returned by hdiutil */
    NSString *dataDev;   /* e.g. /dev/disk7s1  — the APFS Data volume */
    NSString *mountPt;   /* e.g. /Volumes/...Data — where it got mounted */
} MountState;

static void unmount_and_detach(MountState *st) {
    if (st->dataDev) {
        (void)run_tool(@"/usr/sbin/diskutil",
                       @[@"unmount", @"force", st->dataDev], NULL, NULL);
    }
    if (st->diskDev) {
        (void)run_tool(@"/usr/bin/hdiutil",
                       @[@"detach", @"-force", st->diskDev], NULL, NULL);
    }
    st->diskDev = nil;
    st->dataDev = nil;
    st->mountPt = nil;
}

/* Parse `hdiutil attach -plist` output to find the /dev/diskN whole-image
 * parent device (the one carrying the GPT partition table). The hdiutil
 * output lists many devices of the form /dev/diskN (parent) and /dev/diskNsM
 * (partitions), plus virtual-container devices created by APFS probing.
 * Only the GPT parent has content-hint == "GUID_partition_scheme". */
static NSString *parse_first_dev(NSString *plistXml) {
    NSData *data = [plistXml dataUsingEncoding:NSUTF8StringEncoding];
    NSError *err = nil;
    id parsed = [NSPropertyListSerialization propertyListWithData:data
                                                            options:0
                                                             format:NULL
                                                              error:&err];
    if (![parsed isKindOfClass:[NSDictionary class]]) return nil;
    NSArray *sysEntities = parsed[@"system-entities"];
    if (![sysEntities isKindOfClass:[NSArray class]]) return nil;

    for (NSDictionary *e in sysEntities) {
        NSString *hint = e[@"content-hint"];
        NSString *dev  = e[@"dev-entry"];
        if (!dev) continue;
        if ([hint isEqualToString:@"GUID_partition_scheme"] ||
            [hint isEqualToString:@"Apple_partition_scheme"]) {
            return dev;
        }
    }
    /* Fallback: first entry (hdiutil always lists the root device first). */
    NSDictionary *first = sysEntities.firstObject;
    return first[@"dev-entry"];
}

static BOOL mount_disk(NSString *diskImagePath, MountState *out, NSString **errOut) {
    out->diskDev = nil;
    out->dataDev = nil;
    out->mountPt = nil;

    emit_status(@"Attaching disk image");
    NSString *sout = nil, *serr = nil;
    int rc = run_tool(@"/usr/bin/hdiutil",
                      @[@"attach", @"-nomount", @"-readwrite", @"-noverify",
                        @"-plist", diskImagePath],
                      &sout, &serr);
    if (rc != 0) {
        if (errOut) *errOut = [NSString stringWithFormat:@"hdiutil attach failed (%d): %@", rc, serr];
        return NO;
    }
    NSString *parent = parse_first_dev(sout);
    if (!parent) {
        if (errOut) *errOut = @"hdiutil returned no /dev/diskN";
        return NO;
    }
    out->diskDev = parent;
    emit_log([NSString stringWithFormat:@"Attached as %@", parent]);

    /* Find the APFS Data volume under this disk image. Apple Silicon
     * installs produce a GPT with three APFS containers (ISC, Main,
     * Recovery), so we can't derive the container from the parent name.
     * Walk `diskutil apfs list -plist` instead, matching containers by
     * PhysicalStores back to our parent partitions. */
    emit_status(@"Locating Data volume");
    NSString *plistOut = nil;
    rc = run_tool(@"/usr/sbin/diskutil",
                  @[@"apfs", @"list", @"-plist"], &plistOut, &serr);
    if (rc != 0 || plistOut.length == 0) {
        unmount_and_detach(out);
        if (errOut) *errOut = [NSString stringWithFormat:@"diskutil apfs list failed: %@", serr];
        return NO;
    }

    NSData *pd = [plistOut dataUsingEncoding:NSUTF8StringEncoding];
    NSError *perr = nil;
    id parsed = [NSPropertyListSerialization propertyListWithData:pd
                                                             options:0
                                                              format:NULL
                                                               error:&perr];
    if (![parsed isKindOfClass:[NSDictionary class]]) {
        unmount_and_detach(out);
        if (errOut) *errOut = @"diskutil plist parse failed";
        return NO;
    }

    NSString *parentName = [parent lastPathComponent];   /* e.g. "disk4" */
    NSString *prefix = [parentName stringByAppendingString:@"s"]; /* "disk4s" */

    NSString *dataVol = nil;
    for (NSDictionary *container in parsed[@"Containers"]) {
        BOOL ours = NO;
        for (NSDictionary *store in container[@"PhysicalStores"]) {
            NSString *dev = store[@"DeviceIdentifier"];
            if ([dev hasPrefix:prefix]) { ours = YES; break; }
        }
        if (!ours) continue;

        for (NSDictionary *volume in container[@"Volumes"]) {
            NSArray *roles = volume[@"Roles"];
            if ([roles isKindOfClass:[NSArray class]] &&
                [roles containsObject:@"Data"]) {
                dataVol = volume[@"DeviceIdentifier"];
                break;
            }
        }
        if (dataVol) break;
    }

    if (!dataVol) {
        unmount_and_detach(out);
        if (errOut) *errOut = @"No APFS Data-role volume found in image";
        return NO;
    }
    out->dataDev = [NSString stringWithFormat:@"/dev/%@", dataVol];
    emit_log([NSString stringWithFormat:@"Data volume: %@", out->dataDev]);

    emit_status(@"Mounting Data volume");
    NSString *mountOut = nil;
    rc = run_tool(@"/usr/sbin/diskutil",
                  @[@"mount", out->dataDev], &mountOut, &serr);
    if (rc != 0) {
        unmount_and_detach(out);
        if (errOut) *errOut = [NSString stringWithFormat:@"diskutil mount failed: %@", serr];
        return NO;
    }

    /* diskutil prints "Volume <name> on <dev> mounted" — query info for the path. */
    NSString *infoOut = nil;
    rc = run_tool(@"/usr/sbin/diskutil",
                  @[@"info", out->dataDev], &infoOut, &serr);
    if (rc != 0) {
        unmount_and_detach(out);
        if (errOut) *errOut = @"diskutil info failed";
        return NO;
    }
    NSString *mp = nil;
    for (NSString *line in [infoOut componentsSeparatedByString:@"\n"]) {
        NSRange r = [line rangeOfString:@"Mount Point:"];
        if (r.location == NSNotFound) continue;
        NSString *rest = [[line substringFromIndex:r.location + r.length]
                             stringByTrimmingCharactersInSet:
                                 [NSCharacterSet whitespaceCharacterSet]];
        if (rest.length) mp = rest;
        break;
    }
    if (!mp) {
        unmount_and_detach(out);
        if (errOut) *errOut = @"Data volume mount point not found";
        return NO;
    }
    out->mountPt = mp;
    emit_log([NSString stringWithFormat:@"Mounted at %@", mp]);

    /* APFS volumes from a disk image are mounted with ignore-ownership
     * by default — chown(path, 0, 0) will silently no-op. Enable
     * ownership so our root:wheel chown actually persists to disk. */
    rc = run_tool(@"/usr/sbin/diskutil",
                  @[@"enableOwnership", mp], NULL, &serr);
    if (rc != 0) {
        emit_log([NSString stringWithFormat:@"enableOwnership failed (non-fatal): %@", serr]);
    }
    return YES;
}

/* ---- Manifest ---- */

typedef struct {
    /* Non-retaining: these point into autoreleased NSArrays from
     * load_manifest that outlive the whole stage command (the only
     * @autoreleasepool wraps cmd_stage in main). NSValue stores raw bytes and
     * never retains them, so the struct members must not be __strong. */
    __unsafe_unretained NSString *src;
    __unsafe_unretained NSString *destRel;
    mode_t    mode;
    uid_t     uid;
    gid_t     gid;
} StageEntry;

static BOOL parse_owner(NSString *ownerStr, uid_t *uid, gid_t *gid, NSString **errOut) {
    NSArray *parts = [ownerStr componentsSeparatedByString:@":"];
    if (parts.count != 2) {
        if (errOut) *errOut = [NSString stringWithFormat:@"invalid owner '%@'", ownerStr];
        return NO;
    }
    const char *user = [parts[0] UTF8String];
    const char *grp  = [parts[1] UTF8String];
    struct passwd *pw = getpwnam(user);
    struct group  *gr = getgrnam(grp);
    if (!pw || !gr) {
        if (errOut) *errOut = [NSString stringWithFormat:@"unknown user/group '%@'", ownerStr];
        return NO;
    }
    *uid = pw->pw_uid;
    *gid = gr->gr_gid;
    return YES;
}

static NSArray<NSValue *> *load_manifest(NSString *path, NSString **errOut) {
    NSError *rErr = nil;
    NSString *text = [NSString stringWithContentsOfFile:path
                                               encoding:NSUTF8StringEncoding
                                                  error:&rErr];
    if (!text) {
        if (errOut) *errOut = rErr.localizedDescription;
        return nil;
    }
    /* Strip BOM if present. */
    if ([text hasPrefix:@"\uFEFF"]) text = [text substringFromIndex:1];

    NSMutableArray *rows = [NSMutableArray array];
    NSArray *lines = [text componentsSeparatedByString:@"\n"];
    int lineno = 0;
    for (NSString *raw in lines) {
        lineno++;
        NSString *line = [raw stringByTrimmingCharactersInSet:
                             [NSCharacterSet whitespaceCharacterSet]];
        if (line.length == 0 || [line hasPrefix:@"#"]) continue;

        NSArray *fields = [line componentsSeparatedByString:@"\t"];
        if (fields.count != 4) {
            if (errOut) *errOut = [NSString stringWithFormat:
                @"manifest line %d: need 4 tab-separated fields", lineno];
            return nil;
        }
        StageEntry e = {0};
        e.src     = fields[0];
        e.destRel = fields[1];
        e.mode    = (mode_t)strtol([fields[2] UTF8String], NULL, 8);
        NSString *perr = nil;
        if (!parse_owner(fields[3], &e.uid, &e.gid, &perr)) {
            if (errOut) *errOut = [NSString stringWithFormat:
                @"manifest line %d: %@", lineno, perr];
            return nil;
        }
        [rows addObject:[NSValue valueWithBytes:&e objCType:@encode(StageEntry)]];
    }
    return rows;
}

/* ---- Staging ---- */

static BOOL mkdirs_root(NSString *dir, NSString **errOut) {
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:dir]) return YES;
    NSError *err = nil;
    /* createDirectoryAtPath inherits creator uid/gid; we'll chown afterwards if needed. */
    if (![fm createDirectoryAtPath:dir
        withIntermediateDirectories:YES
                         attributes:@{NSFilePosixPermissions: @(0755)}
                              error:&err]) {
        if (errOut) *errOut = err.localizedDescription;
        return NO;
    }
    return YES;
}

static BOOL stage_entry(StageEntry e, NSString *mountPt, NSString **errOut) {
    NSString *dst = [mountPt stringByAppendingPathComponent:e.destRel];
    NSString *dstDir = [dst stringByDeletingLastPathComponent];

    if (!mkdirs_root(dstDir, errOut)) return NO;

    NSFileManager *fm = [NSFileManager defaultManager];
    [fm removeItemAtPath:dst error:nil];
    NSError *cpErr = nil;
    if (![fm copyItemAtPath:e.src toPath:dst error:&cpErr]) {
        if (errOut) *errOut = [NSString stringWithFormat:@"copy %@ -> %@: %@",
                               e.src, e.destRel, cpErr.localizedDescription];
        return NO;
    }
    if (removexattr(dst.fileSystemRepresentation, "com.apple.quarantine", 0) != 0 &&
        errno != ENOATTR) {
        if (errOut) *errOut = [NSString stringWithFormat:@"remove quarantine %@: %s",
                               e.destRel, strerror(errno)];
        return NO;
    }
    if (chown(dst.fileSystemRepresentation, e.uid, e.gid) != 0) {
        if (errOut) *errOut = [NSString stringWithFormat:@"chown %@: %s",
                               e.destRel, strerror(errno)];
        return NO;
    }
    if (chmod(dst.fileSystemRepresentation, e.mode) != 0) {
        if (errOut) *errOut = [NSString stringWithFormat:@"chmod %@: %s",
                               e.destRel, strerror(errno)];
        return NO;
    }
    return YES;
}

static BOOL write_plist(NSDictionary *plist, NSString *path,
                         uid_t uid, gid_t gid, mode_t mode,
                         NSString **errOut) {
    NSError *e = nil;
    NSData *d = [NSPropertyListSerialization dataWithPropertyList:plist
                                                            format:NSPropertyListXMLFormat_v1_0
                                                           options:0
                                                             error:&e];
    if (!d) {
        if (errOut) *errOut = [NSString stringWithFormat:@"plist serialize: %@",
                               e.localizedDescription];
        return NO;
    }
    [[NSFileManager defaultManager] createDirectoryAtPath:
        [path stringByDeletingLastPathComponent]
                                withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
    if (![d writeToFile:path atomically:YES]) {
        if (errOut) *errOut = [NSString stringWithFormat:@"write %@", path];
        return NO;
    }
    chown(path.fileSystemRepresentation, uid, gid);
    chmod(path.fileSystemRepresentation, mode);
    return YES;
}

/* Enable the built-in OpenSSH daemon by clearing its Disabled flag in
 * launchd's override database. This works offline (disk mounted on the
 * host) and survives reboots, which `systemsetup -setremotelogin on`
 * does not from a LaunchDaemon context on modern macOS (requires Full
 * Disk Access through TCC that root LaunchDaemons don't have). */
static BOOL enable_ssh(NSString *mountPt, NSString **errOut) {
    emit_status(@"Enabling SSH (launchd override)");

    NSString *path = [mountPt stringByAppendingPathComponent:
        @"private/var/db/com.apple.xpc.launchd/disabled.plist"];
    NSMutableDictionary *d =
        [[NSDictionary dictionaryWithContentsOfFile:path] mutableCopy];
    if (!d) {
        /* Alternate layout without the /private firmlink indirection. */
        NSString *alt = [mountPt stringByAppendingPathComponent:
            @"var/db/com.apple.xpc.launchd/disabled.plist"];
        d = [[NSDictionary dictionaryWithContentsOfFile:alt] mutableCopy];
        if (d) path = alt;
    }
    if (!d) d = [NSMutableDictionary dictionary];

    d[@"com.openssh.sshd"] = @NO;   /* NO = not disabled */
    return write_plist(d, path, 0, 0, 0644, errOut);
}

/* Drop a sidecar file with the user's chosen display name so firstboot.sh
 * can apply it via `scutil --set` on first boot. Writing the SystemConfig
 * preferences.plist offline does not work: configd owns that file and
 * regenerates it at boot from its own state, falling back to the hardware
 * model name ("Apple Virtual Machine") if it never saw a live API call.
 * The canonical path is SCDynamicStoreSetComputerName /
 * SCPreferencesSetLocalHostName, which scutil wraps. */
static BOOL set_computer_name(NSString *mountPt, NSString *displayName,
                              NSString **errOut) {
    emit_status([NSString stringWithFormat:@"Queueing computer name (%@)",
                                            displayName]);

    NSString *dir = [mountPt stringByAppendingPathComponent:
        @"Library/AppSandbox"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil error:nil];

    NSString *path = [dir stringByAppendingPathComponent:@"computer-name"];
    NSError *err = nil;
    if (![displayName writeToFile:path atomically:YES
                         encoding:NSUTF8StringEncoding error:&err]) {
        if (errOut) *errOut = err.localizedDescription;
        return NO;
    }
    chown(path.fileSystemRepresentation, 0, 0);
    chmod(path.fileSystemRepresentation, 0644);
    return YES;
}

static MountState g_mount = {0};

/* ---- "stage" subcommand ---- */

static int cmd_stage(int argc, char **argv) {
    NSString *diskPath = nil;
    NSString *manifest = nil;
    NSString *computerName = nil;
    NSString *waitForUser = nil;
    uint64_t diskBytes = 0;
    BOOL sshOn = NO;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc)
            diskPath = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--disk-gb") == 0 && i + 1 < argc)
            diskBytes = strtoull(argv[++i], NULL, 10) * 1024 * 1024 * 1024;
        else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc)
            manifest = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--computer-name") == 0 && i + 1 < argc)
            computerName = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--wait-for-user") == 0 && i + 1 < argc)
            waitForUser = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--enable-ssh") == 0)
            sshOn = YES;
    }
    if (!diskPath || !manifest) {
        emit_error(@"stage: --disk and --manifest required");
        return 2;
    }
    if (geteuid() != 0) {
        emit_error(@"stage: must run through the elevated AppSandbox core");
        return 2;
    }

    NSString *err = nil;
    NSArray *entries = load_manifest(manifest, &err);
    if (!entries) { emit_error(err); return 3; }

    emit_progress(5, @"Loaded manifest");

    struct stat diskStat;
    if (stat(diskPath.fileSystemRepresentation, &diskStat) != 0) {
        emit_error([NSString stringWithFormat:@"Cannot read virtual disk size: %s", strerror(errno)]);
        return 4;
    }
    if (diskBytes > (uint64_t)diskStat.st_size) {
        emit_status(@"Expanding macOS disk");
        NSString *resizeError = nil;
        if (run_tool(@"/usr/sbin/diskutil", @[@"image", @"resize", @"--size",
            [NSString stringWithFormat:@"%llu", (unsigned long long)diskBytes], diskPath], NULL, &resizeError) != 0) {
            NSString *detail = resizeError ?: @"diskutil failed";
            emit_error([@"Failed to expand macOS disk: " stringByAppendingString:
                [detail stringByReplacingOccurrencesOfString:@"\n" withString:@" "]]);
            return 4;
        }
    }

    if (!mount_disk(diskPath, &g_mount, &err)) {
        emit_error(err);
        return 4;
    }

    emit_progress(30, @"Staging files");
    int total = (int)entries.count;
    int fi = 0;
    for (NSValue *v in entries) {
        StageEntry e = {0};
        [v getValue:&e];
        fi++;
        emit_progress(30 + (40 * fi / MAX(total, 1)),
                      [NSString stringWithFormat:@"Copying %@", e.destRel]);
        if (!stage_entry(e, g_mount.mountPt, &err)) {
            unmount_and_detach(&g_mount);
            emit_error(err);
            return 5;
        }
    }

    if (waitForUser.length) {
        NSString *base = [g_mount.mountPt stringByAppendingPathComponent:@"Library/AppSandbox"];
        NSDictionary *settings = @{@"firstboot-user": waitForUser,
                                    @"enable-ssh": sshOn ? @"1" : @"0"};
        for (NSString *name in settings) {
            NSString *path = [base stringByAppendingPathComponent:name];
            if (![settings[name] writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
                unmount_and_detach(&g_mount);
                emit_error(@"stage: cannot write first-boot settings");
                return 6;
            }
            chown(path.fileSystemRepresentation, 0, 0);
            chmod(path.fileSystemRepresentation, 0644);
        }
    }

    /* Enable SSH only if the user opted in at create time. When enabled,
     * we flip launchd's disabled.plist offline so sshd is on from first
     * boot, and the host-side vsock SSH proxy exposes it on a loopback
     * port. When disabled, sshd stays off. */
    if (sshOn) {
        emit_progress(93, @"Enabling SSH");
        if (!enable_ssh(g_mount.mountPt, &err)) {
            unmount_and_detach(&g_mount);
            emit_error(err);
            return 9;
        }
    }

    if (computerName.length) {
        emit_progress(94, @"Setting computer name");
        if (!set_computer_name(g_mount.mountPt, computerName, &err)) {
            unmount_and_detach(&g_mount);
            emit_error(err);
            return 10;
        }
    }

    emit_progress(95, @"Unmounting");
    unmount_and_detach(&g_mount);

    emit_progress(100, @"Done");
    emit_done(diskPath);
    return 0;
}

/* ---- NSURLSession download delegate (fetch-ipsw) ---- */

@interface IPSWDownloadDelegate : NSObject <NSURLSessionDownloadDelegate>
@property (nonatomic, copy)   NSString                 *outputPath;
@property (nonatomic, copy)   NSString                 *sidecarPath;
@property (nonatomic, copy)   NSURL                    *sourceURL;
@property (nonatomic, strong) NSURLSession             *session;
@property (nonatomic, strong) NSURLSessionDownloadTask *currentTask;
@property (nonatomic, copy)   void (^completionHandler)(NSError *);
@property (nonatomic, assign) BOOL                      reportedSize;
@property (nonatomic, assign) int                       lastPct;
@end

@implementation IPSWDownloadDelegate

/* Sidecar: 8-byte LE URL length, URL bytes, resume-data blob. */
- (void)persistResumeData:(NSData *)data {
    if (!data || !self.sidecarPath || !self.sourceURL) return;
    NSData *urlData = [self.sourceURL.absoluteString dataUsingEncoding:NSUTF8StringEncoding];
    uint64_t n = (uint64_t)urlData.length;
    NSMutableData *out = [NSMutableData dataWithCapacity:8 + urlData.length + data.length];
    [out appendBytes:&n length:sizeof(n)];
    [out appendData:urlData];
    [out appendData:data];
    [out writeToFile:self.sidecarPath atomically:YES];
}

- (void)clearSidecar {
    if (self.sidecarPath) {
        [[NSFileManager defaultManager] removeItemAtPath:self.sidecarPath error:nil];
    }
}

+ (NSData *)resumeDataFromSidecarAt:(NSString *)path matchingURL:(NSURL *)url {
    NSData *raw = [NSData dataWithContentsOfFile:path];
    if (raw.length < 8) return nil;
    uint64_t n = 0;
    [raw getBytes:&n length:sizeof(n)];
    /* raw.length >= 8 is guaranteed above, so (raw.length - 8) can't underflow;
     * compare with subtraction rather than 8 + n. */
    if (n == 0 || n > raw.length - 8) return nil;
    NSData *urlData = [raw subdataWithRange:NSMakeRange(8, (NSUInteger)n)];
    NSString *savedURL = [[NSString alloc] initWithData:urlData encoding:NSUTF8StringEncoding];
    if (![savedURL isEqualToString:url.absoluteString]) return nil;
    return [raw subdataWithRange:NSMakeRange(8 + (NSUInteger)n,
                                             raw.length - 8 - (NSUInteger)n)];
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)task
      didWriteData:(int64_t)bytes
 totalBytesWritten:(int64_t)totalWritten
totalBytesExpectedToWrite:(int64_t)totalExpected {
    (void)session; (void)task; (void)bytes;
    if (totalExpected <= 0) return;
    if (!self.reportedSize) {
        self.reportedSize = YES;
        emit_log([NSString stringWithFormat:@"Restore image size: %.1f GB",
                  (double)totalExpected / 1073741824.0]);
    }
    int pct = (int)(100.0 * (double)totalWritten / (double)totalExpected);
    if (pct == self.lastPct) return;
    self.lastPct = pct;
    emit_progress(pct, [NSString stringWithFormat:@"Downloading (%d%%)", pct]);
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)task
 didResumeAtOffset:(int64_t)fileOffset
expectedTotalBytes:(int64_t)expectedTotalBytes {
    (void)session; (void)task;
    int pct = (expectedTotalBytes > 0)
        ? (int)(100.0 * (double)fileOffset / (double)expectedTotalBytes)
        : self.lastPct;
    self.lastPct = pct;
    emit_progress(pct, [NSString stringWithFormat:@"Resuming download (%d%%)", pct]);
}

- (void)URLSession:(NSURLSession *)session
taskIsWaitingForConnectivity:(NSURLSessionTask *)task {
    (void)session; (void)task;
    emit_progress(self.lastPct,
        [NSString stringWithFormat:@"Waiting for network (%d%%)", self.lastPct]);
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)task
didFinishDownloadingToURL:(NSURL *)location {
    (void)session; (void)task;
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm removeItemAtPath:self.outputPath error:nil];
    NSError *err = nil;
    if (![fm moveItemAtPath:location.path toPath:self.outputPath error:&err]) {
        if (self.completionHandler) { self.completionHandler(err); self.completionHandler = nil; }
        return;
    }
    [self clearSidecar];
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
    (void)session; (void)task;

    if (!error) {
        if (self.completionHandler) { self.completionHandler(nil); self.completionHandler = nil; }
        return;
    }

    if (error.code == NSURLErrorCancelled) {
        if (self.completionHandler) { self.completionHandler(error); self.completionHandler = nil; }
        return;
    }

    NSData *resumeData = error.userInfo[NSURLSessionDownloadTaskResumeData];
    if (resumeData) [self persistResumeData:resumeData];

    emit_log([NSString stringWithFormat:@"Download interrupted (%@); reconnecting",
              error.localizedDescription]);
    emit_progress(self.lastPct,
        [NSString stringWithFormat:@"Reconnecting (%d%%)", self.lastPct]);

    NSURLSessionDownloadTask *next = resumeData
        ? [self.session downloadTaskWithResumeData:resumeData]
        : [self.session downloadTaskWithURL:self.sourceURL];
    self.currentTask = next;
    [next resume];
}
@end

/* ---- Install progress observer ---- */

@interface InstallProgressObserver : NSObject
@property (nonatomic, strong) NSProgress *observed;
@property (nonatomic, assign) int lastPct;
@end
@implementation InstallProgressObserver
- (instancetype)init { self = [super init]; if (self) _lastPct = -1; return self; }
- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary *)change
                       context:(void *)context {
    (void)object; (void)change; (void)context;
    if (![keyPath isEqualToString:@"fractionCompleted"]) return;
    int pct = (int)(self.observed.fractionCompleted * 100.0);
    if (pct == self.lastPct) return;
    self.lastPct = pct;
    emit_progress(pct, [NSString stringWithFormat:@"Installing macOS (%d%%)", pct]);
}
@end

/* ---- "fetch-ipsw" subcommand ---- */

static int cmd_fetch_ipsw(int argc, char **argv) {
    NSString *outPath = nil;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            outPath = [NSString stringWithUTF8String:argv[++i]];
    }
    if (!outPath) { emit_error(@"fetch-ipsw: --output required"); return 2; }

    NSString *sidecar = [outPath stringByAppendingPathExtension:@"resume"];
    __block int exitCode = 0;
    __block IPSWDownloadDelegate *activeDelegate = nil;

    /* On SIGTERM, persist resume data so a later run can pick up here. */
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t sigSrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
                                                       SIGTERM, 0,
                                                       dispatch_get_main_queue());
    dispatch_source_set_event_handler(sigSrc, ^{
        IPSWDownloadDelegate *d = activeDelegate;
        if (!d.currentTask) {
            CFRunLoopStop(CFRunLoopGetMain());
            return;
        }
        [d.currentTask cancelByProducingResumeData:^(NSData * _Nullable data) {
            if (data) [d persistResumeData:data];
            CFRunLoopStop(CFRunLoopGetMain());
        }];
    });
    dispatch_resume(sigSrc);

    dispatch_async(dispatch_get_main_queue(), ^{
        emit_status(@"Querying Apple for latest restore image");
        [VZMacOSRestoreImage fetchLatestSupportedWithCompletionHandler:
            ^(VZMacOSRestoreImage * _Nullable img, NSError * _Nullable err) {
            if (err || !img) {
                emit_error(err.localizedDescription ?: @"no supported restore image");
                exitCode = 3;
                CFRunLoopStop(CFRunLoopGetMain());
                return;
            }
            emit_log([NSString stringWithFormat:@"Source: %@", img.URL.absoluteString]);

            /* Only reuse the sidecar if Apple hasn't rotated the IPSW URL. */
            NSData *priorResume = [IPSWDownloadDelegate
                resumeDataFromSidecarAt:sidecar matchingURL:img.URL];
            if (priorResume) {
                emit_log(@"Found prior resume data; attempting to resume");
            } else if ([[NSFileManager defaultManager] fileExistsAtPath:sidecar]) {
                [[NSFileManager defaultManager] removeItemAtPath:sidecar error:nil];
                emit_log(@"Discarded stale resume data (source URL changed)");
            }

            IPSWDownloadDelegate *del = [[IPSWDownloadDelegate alloc] init];
            del.outputPath  = outPath;
            del.sidecarPath = sidecar;
            del.sourceURL   = img.URL;
            del.completionHandler = ^(NSError *dlErr) {
                if (dlErr) {
                    emit_error(dlErr.localizedDescription);
                    exitCode = 4;
                } else {
                    emit_done(outPath);
                }
                CFRunLoopStop(CFRunLoopGetMain());
            };
            NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration defaultSessionConfiguration];
            cfg.timeoutIntervalForRequest  = 0;
            cfg.timeoutIntervalForResource = 0;
            cfg.waitsForConnectivity       = YES;
            NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg
                                                                  delegate:del
                                                             delegateQueue:nil];
            del.session = session;

            NSURLSessionDownloadTask *task = priorResume
                ? [session downloadTaskWithResumeData:priorResume]
                : [session downloadTaskWithURL:img.URL];
            del.currentTask = task;
            activeDelegate  = del;
            [task resume];
        }];
    });

    CFRunLoopRun();
    return exitCode;
}

/* ---- "install" subcommand ---- */

/* Hold installer + observer across the async call so ARC doesn't release them. */
static VZMacOSInstaller        *g_installer = nil;
static InstallProgressObserver *g_progressObs = nil;
static VZVirtualMachine        *g_installVM = nil;

static int cmd_install(int argc, char **argv) {
    NSString *name = nil, *vmDir = nil, *ipsw = nil, *diskPath = nil;
    NSString *macAddress = nil;
    int ramMb = 0, cpus = 0, diskGb = 0;
    int networkFd = -1;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = @(argv[++i]);
        else if (strcmp(argv[i], "--vm-dir") == 0 && i + 1 < argc) vmDir = @(argv[++i]);
        else if (strcmp(argv[i], "--disk-path") == 0 && i + 1 < argc) diskPath = @(argv[++i]);
        else if (strcmp(argv[i], "--ipsw") == 0 && i + 1 < argc) ipsw = @(argv[++i]);
        else if (strcmp(argv[i], "--ram-mb") == 0 && i + 1 < argc) ramMb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cpus") == 0 && i + 1 < argc) cpus = atoi(argv[++i]);
        else if (strcmp(argv[i], "--disk-gb") == 0 && i + 1 < argc) diskGb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--mac-address") == 0 && i + 1 < argc) macAddress = @(argv[++i]);
        else if (strcmp(argv[i], "--network-fd") == 0 && i + 1 < argc) {
            if (strcmp(argv[++i], "0") == 0) networkFd = STDIN_FILENO;
        }
    }
    if (!name || !vmDir || !ipsw || ramMb <= 0 || cpus <= 0 || diskGb <= 0 || networkFd < 0) {
        emit_error(@"install: --name --vm-dir --ipsw --ram-mb --cpus --disk-gb --network-fd all required");
        return 2;
    }
    int socketType = 0;
    socklen_t typeLength = sizeof(socketType), peerLength = sizeof(struct sockaddr_storage);
    struct sockaddr_storage peer;
    if (getsockopt(networkFd, SOL_SOCKET, SO_TYPE, &socketType, &typeLength) != 0 ||
        socketType != SOCK_DGRAM ||
        getpeername(networkFd, (struct sockaddr *)&peer, &peerLength) != 0 || peer.ss_family != AF_UNIX) {
        emit_error(@"install: a connected core-owned datagram network socket is required");
        return 2;
    }
    NSFileHandle *networkHandle = [[NSFileHandle alloc] initWithFileDescriptor:networkFd closeOnDealloc:NO];
    VZMACAddress *mac = macAddress ? [[VZMACAddress alloc] initWithString:macAddress] : nil;
    if (macAddress && !mac.isUnicastAddress) {
        emit_error(@"install: invalid MAC address");
        return 2;
    }

    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *mkErr = nil;
    if (![fm createDirectoryAtPath:vmDir withIntermediateDirectories:YES
                        attributes:nil error:&mkErr]) {
        emit_error([NSString stringWithFormat:@"mkdir %@: %@", vmDir, mkErr.localizedDescription]);
        return 3;
    }

    if (!diskPath) diskPath = [vmDir stringByAppendingPathComponent:@"disk.img"];
    NSString *auxPath  = [vmDir stringByAppendingPathComponent:@"aux.img"];
    NSString *hwPath   = [vmDir stringByAppendingPathComponent:@"hardware.bin"];
    NSString *midPath  = [vmDir stringByAppendingPathComponent:@"machine-id.bin"];

    emit_status(@"Creating disk image");
    uint64_t sizeBytes = (uint64_t)diskGb * 1024ULL * 1024ULL * 1024ULL;
    int dfd = open(diskPath.fileSystemRepresentation,
                   O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (dfd < 0) {
        emit_error([NSString stringWithFormat:@"open disk.img: %s", strerror(errno)]);
        return 4;
    }
    if (ftruncate(dfd, (off_t)sizeBytes) != 0) {
        int e = errno; close(dfd);
        emit_error([NSString stringWithFormat:@"ftruncate disk.img: %s", strerror(e)]);
        return 4;
    }
    close(dfd);
    emit_progress(5, @"Disk image created");

    __block int exitCode = 0;

    dispatch_async(dispatch_get_main_queue(), ^{
        emit_status(@"Loading restore image");
        [VZMacOSRestoreImage loadFileURL:[NSURL fileURLWithPath:ipsw]
                        completionHandler:^(VZMacOSRestoreImage * _Nullable img,
                                            NSError * _Nullable loadErr) {
            /* loadFileURL's completion fires on an internal queue. All
             * subsequent VZ APIs (VZVirtualMachine, VZMacOSInstaller,
             * VZMacAuxiliaryStorage) require the main queue. */
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!img) {
                    emit_error(loadErr.localizedDescription ?: @"failed to load IPSW");
                    exitCode = 5;
                    CFRunLoopStop(CFRunLoopGetMain());
                    return;
                }

                VZMacOSConfigurationRequirements *reqs = img.mostFeaturefulSupportedConfiguration;
                if (!reqs) {
                    emit_error(@"restore image has no supported configuration on this host");
                    exitCode = 6;
                    CFRunLoopStop(CFRunLoopGetMain());
                    return;
                }

                int finalCpus = MAX(cpus, (int)reqs.minimumSupportedCPUCount);
                uint64_t ramBytes = MAX((uint64_t)ramMb * 1024ULL * 1024ULL,
                                        reqs.minimumSupportedMemorySize);

                VZMacHardwareModel *hw = reqs.hardwareModel;
                [hw.dataRepresentation writeToFile:hwPath atomically:YES];

                VZMacMachineIdentifier *mid = [[VZMacMachineIdentifier alloc] init];
                [mid.dataRepresentation writeToFile:midPath atomically:YES];

                NSError *auxErr = nil;
                VZMacAuxiliaryStorage *aux = [[VZMacAuxiliaryStorage alloc]
                    initCreatingStorageAtURL:[NSURL fileURLWithPath:auxPath]
                               hardwareModel:hw
                                     options:VZMacAuxiliaryStorageInitializationOptionAllowOverwrite
                                       error:&auxErr];
                if (!aux) {
                    emit_error(auxErr.localizedDescription ?: @"aux storage creation failed");
                    exitCode = 7;
                    CFRunLoopStop(CFRunLoopGetMain());
                    return;
                }

                NSError *attErr = nil;
                VZDiskImageStorageDeviceAttachment *att =
                    [[VZDiskImageStorageDeviceAttachment alloc]
                        initWithURL:[NSURL fileURLWithPath:diskPath]
                            readOnly:NO error:&attErr];
                if (!att) {
                    emit_error(attErr.localizedDescription ?: @"disk attach failed");
                    exitCode = 8;
                    CFRunLoopStop(CFRunLoopGetMain());
                    return;
                }

                VZVirtualMachineConfiguration *config = [[VZVirtualMachineConfiguration alloc] init];
                config.CPUCount   = (NSUInteger)finalCpus;
                config.memorySize = ramBytes;

                VZMacPlatformConfiguration *platform = [[VZMacPlatformConfiguration alloc] init];
                platform.hardwareModel      = hw;
                platform.machineIdentifier  = mid;
                platform.auxiliaryStorage   = aux;
                config.platform   = platform;
                config.bootLoader = [[VZMacOSBootLoader alloc] init];
                config.storageDevices = @[[[VZVirtioBlockDeviceConfiguration alloc]
                                              initWithAttachment:att]];

                VZMacGraphicsDeviceConfiguration *gfx = [[VZMacGraphicsDeviceConfiguration alloc] init];
                gfx.displays = @[[[VZMacGraphicsDisplayConfiguration alloc]
                                      initWithWidthInPixels:2560
                                             heightInPixels:1600
                                              pixelsPerInch:144]];
                config.graphicsDevices = @[gfx];

                config.keyboards        = @[[[VZUSBKeyboardConfiguration alloc] init]];
                config.pointingDevices  = @[[[VZMacTrackpadConfiguration alloc] init],
                                            [[VZUSBScreenCoordinatePointingDeviceConfiguration alloc] init]];

                VZVirtioNetworkDeviceConfiguration *netCfg = [[VZVirtioNetworkDeviceConfiguration alloc] init];
                if (mac) netCfg.MACAddress = mac;
                netCfg.attachment = [[VZFileHandleNetworkDeviceAttachment alloc] initWithFileHandle:networkHandle];
                config.networkDevices = @[netCfg];

                config.socketDevices = @[[[VZVirtioSocketDeviceConfiguration alloc] init]];

                NSError *valErr = nil;
                if (![config validateWithError:&valErr]) {
                    emit_error(valErr.localizedDescription ?: @"invalid VM config");
                    exitCode = 9;
                    CFRunLoopStop(CFRunLoopGetMain());
                    return;
                }

                g_installVM = [[VZVirtualMachine alloc] initWithConfiguration:config];
                g_installer = [[VZMacOSInstaller alloc]
                                   initWithVirtualMachine:g_installVM
                                           restoreImageURL:[NSURL fileURLWithPath:ipsw]];

                g_progressObs = [[InstallProgressObserver alloc] init];
                g_progressObs.observed = g_installer.progress;
                [g_installer.progress addObserver:g_progressObs
                                       forKeyPath:@"fractionCompleted"
                                          options:NSKeyValueObservingOptionNew
                                          context:NULL];

                emit_status(@"Installing macOS");
                [g_installer installWithCompletionHandler:^(NSError * _Nullable installErr) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        @try {
                            [g_installer.progress removeObserver:g_progressObs
                                                      forKeyPath:@"fractionCompleted"];
                        } @catch (__unused NSException *ex) {}
                        if (installErr) {
                            emit_error(installErr.localizedDescription);
                            exitCode = 10;
                        } else {
                            emit_progress(100, @"Install complete");
                            emit_done(diskPath);
                        }
                        CFRunLoopStop(CFRunLoopGetMain());
                    });
                }];
            });
        }];
    });

    CFRunLoopRun();
    g_installer = nil;
    g_progressObs = nil;
    g_installVM = nil;
    return exitCode;
}

/* ======================================================================== *
 *  "build-windows" subcommand
 *
 *  From-scratch Windows 11 ARM64 VM disk creation on macOS. Mirrors the Windows
 *  VHDX-first deploy path (generate_unattend_vhdx + generate_vhdx_setup_cmd +
 *  generate_vhdx_setupcomplete + generate_vhdx_manifest) but writes our OWN NTFS
 *  via win_disk_build() instead of DISM:
 *    1. mount the Microsoft ISO read-only -> sources/install.wim
 *    2. generate unattend.xml + setup.cmd + SetupComplete.cmd into a temp dir
 *    3. build a "<host-src>\t<guest-dest>" manifest: answer file at
 *       \Windows\Panther, agent EXEs + test-signed drivers + devcon at
 *       \Windows\AppSandbox[\drivers], SetupComplete.cmd at \Windows\Setup\Scripts
 *    4. win_disk_build() applies the WIM image then stages our files at those
 *       real guest paths (mkdir -p over the apply-built dir map)
 *    5. unmount the ISO + remove the temp dir
 *
 *  The SetupComplete.cmd differs from the Windows one in one way: it also installs
 *  the AppSandboxSHM PCI driver (pnputil), since the Mac transport is ivshmem rather
 *  than AF_HYPERV. testMode (Secure Boot off + test signing on) is mandatory: the
 *  guest drivers are test-signed. ======================================================================== */

/* Append "<src>\t<dest>\n" to the manifest if src exists on the host. */
static int bw_manifest_add(FILE *mf, NSString *src, const char *dest) {
    if (!src || ![[NSFileManager defaultManager] fileExistsAtPath:src]) return 0;
    fprintf(mf, "%s\t%s\n", src.UTF8String, dest);
    return 1;
}

/* ---- ISO mount lifecycle ---- */
typedef struct { NSString *diskDev; NSString *mountPt; NSString *shadowPath; } IsoMount;
static IsoMount g_iso_mount;
static NSString *g_build_tmp;

/* Attach the ISO and locate the mount point that actually contains sources/install.wim (a Windows ISO
 * exposes both ISO9660 and UDF volumes).
 *
 * CONCURRENCY: each build attaches with its OWN unique copy-on-write SHADOW file, which gives it an
 * INDEPENDENT device + mount point. hdiutil de-duplicates a plain re-attach of the same image to one
 * shared device, so without the shadow two concurrent builds would share a device and detaching one
 * would tear the ISO out from under the other. The shadow stays ~empty (we only READ install.wim) and
 * is deleted on unmount. */
static BOOL bw_mount_iso(NSString *isoPath, IsoMount *out, NSString **errOut) {
    out->diskDev = nil; out->mountPt = nil; out->shadowPath = nil;
    NSString *shadow = [NSTemporaryDirectory() stringByAppendingPathComponent:
                        [NSString stringWithFormat:@"asb-iso-%08x.shadow", arc4random()]];
    NSString *sout = nil, *serr = nil;
    int rc = run_tool(@"/usr/bin/hdiutil",
                      @[@"attach", @"-nobrowse", @"-shadow", shadow, @"-plist", isoPath],
                      &sout, &serr);
    if (rc != 0) { if (errOut) *errOut = [NSString stringWithFormat:@"hdiutil attach failed (%d): %@", rc, serr]; return NO; }
    out->shadowPath = shadow;

    NSData *d = [sout dataUsingEncoding:NSUTF8StringEncoding];
    id parsed = [NSPropertyListSerialization propertyListWithData:d options:0 format:NULL error:NULL];
    NSArray *ents = [parsed isKindOfClass:[NSDictionary class]] ? parsed[@"system-entities"] : nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSDictionary *e in ents) {
        NSString *dev = e[@"dev-entry"];
        /* the whole-image parent has no "s<N>" suffix: record it for detach */
        if (dev && !out->diskDev &&
            [dev rangeOfString:@"s" options:0 range:NSMakeRange(@"/dev/disk".length, dev.length-@"/dev/disk".length)].location == NSNotFound)
            out->diskDev = dev;
        NSString *mp = e[@"mount-point"];
        if (mp.length) {
            NSString *wim = [mp stringByAppendingPathComponent:@"sources/install.wim"];
            if ([fm fileExistsAtPath:wim]) out->mountPt = mp;
        }
    }
    /* Fallback for detach: first dev-entry. */
    if (!out->diskDev && ents.count) out->diskDev = [ents firstObject][@"dev-entry"];
    if (!out->mountPt) {
        /* No install.wim found at any mount point; detach + fail. */
        if (out->diskDev) (void)run_tool(@"/usr/bin/hdiutil", @[@"detach", @"-force", out->diskDev], NULL, NULL);
        [[NSFileManager defaultManager] removeItemAtPath:shadow error:nil];
        out->diskDev = nil;
        out->shadowPath = nil;
        if (errOut) *errOut = @"ISO mounted but sources/install.wim not found";
        return NO;
    }
    return YES;
}

static void bw_unmount_iso(IsoMount *m) {
    if (m->diskDev) (void)run_tool(@"/usr/bin/hdiutil", @[@"detach", @"-force", m->diskDev], NULL, NULL);
    if (m->shadowPath) [[NSFileManager defaultManager] removeItemAtPath:m->shadowPath error:nil];   /* COW shadow */
    m->diskDev = nil; m->mountPt = nil; m->shadowPath = nil;
}

static void cleanup_worker(void) {
    g_cleaning_up = YES;
    unmount_and_detach(&g_mount);
    bw_unmount_iso(&g_iso_mount);
    if (g_build_tmp) [[NSFileManager defaultManager] removeItemAtPath:g_build_tmp error:nil];
}

/* win_disk_build progress -> PROGRESS/STATUS lines. */
static void bw_progress(int pct, const char *msg) {
    emit_progress(pct, msg ? [NSString stringWithUTF8String:msg] : @"");
}

static int cmd_build_windows(int argc, char **argv) {
    NSString *iso = nil, *out = nil, *sshMsi = nil, *signedZip = nil, *netkvmZip = nil;
    NSString *vmName = @"WIN11-VM", *user = @"user", *pass = @"test123";
    NSString *provDir = nil;   /* host-generated unattend.xml/setup.cmd/SetupComplete.cmd (password encoded host-side) */
    NSString *lang = @"";   /* empty => auto-detect from the install.wim XML (else en-US) */
    BOOL isTemplate = NO, resourcesOnly = NO;
    int diskGb = 64, imageIdx = 1, testMode = 1;   /* testMode optional (mirrors Windows disk builder) */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--template") == 0) isTemplate = YES;
        else if (strcmp(argv[i], "--resources-iso") == 0) resourcesOnly = YES;
        else if (strcmp(argv[i], "--iso") == 0 && i+1 < argc) iso = @(argv[++i]);
        else if (strcmp(argv[i], "--out") == 0 && i+1 < argc) out = @(argv[++i]);
        else if (strcmp(argv[i], "--signed-payload-zip") == 0 && i+1 < argc) signedZip = @(argv[++i]);
        else if (strcmp(argv[i], "--netkvm-zip") == 0 && i+1 < argc) netkvmZip = @(argv[++i]);
        else if (strcmp(argv[i], "--ssh-msi") == 0 && i+1 < argc) sshMsi = @(argv[++i]);
        else if (strcmp(argv[i], "--vm-name") == 0 && i+1 < argc) vmName = @(argv[++i]);
        else if (strcmp(argv[i], "--user") == 0 && i+1 < argc) user = @(argv[++i]);
        else if (strcmp(argv[i], "--pass") == 0 && i+1 < argc) pass = @(argv[++i]);
        else if (strcmp(argv[i], "--prov-dir") == 0 && i+1 < argc) provDir = @(argv[++i]);
        else if (strcmp(argv[i], "--lang") == 0 && i+1 < argc) lang = @(argv[++i]);
        else if (strcmp(argv[i], "--disk-gb") == 0 && i+1 < argc) diskGb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--image-index") == 0 && i+1 < argc) imageIdx = atoi(argv[++i]);
        else if (strcmp(argv[i], "--test-mode") == 0 && i+1 < argc) testMode = atoi(argv[++i]);
    }
    if ((!resourcesOnly && !iso) || !out || !signedZip || (resourcesOnly && !provDir)) {
        emit_error(@"build-windows: --iso, --out and --signed-payload-zip are required");
        return 2;
    }
    NSFileManager *fm = [NSFileManager defaultManager];

    /* 1) temp staging dir for the generated answer file + scripts */
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:
                     [NSString stringWithFormat:@"asb-winbuild-%d", getpid()]];
    g_build_tmp = tmp;
    [fm removeItemAtPath:tmp error:nil];
    NSError *mkErr = nil;
    if (![fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:&mkErr]) {
        emit_error([NSString stringWithFormat:@"mkdir staging: %@", mkErr.localizedDescription]); return 3;
    }

    /* The guest payload comes ONLY from the downloaded, signed release zip -- there is no bundled
     * fallback. Extract the whole zip into this per-build temp (its own tmp, so concurrent creates
     * never collide). A full extract (not a path-filtered subset) is required because the Windows-built
     * zip stores entries with backslash separators that forward-slash unzip include-patterns never
     * match (unzip would exit 11 and extract nothing); a full extract makes Info-ZIP rewrite the
     * backslashes into real resources/ + drivers/ subdirs. Such zips make unzip exit 1 (warning), so
     * the exit code is not a reliable signal -- the probe file decides. binDir holds the signed EXEs;
     * drvDir holds the signed drivers (VDD/VAD/SHM) + devcon. */
    NSString *signedDir = [tmp stringByAppendingPathComponent:@"signed"];
    (void)run_tool(@"/usr/bin/unzip", @[@"-o", @"-q", signedZip, @"-d", signedDir], NULL, NULL);
    NSString *binDir = [signedDir stringByAppendingPathComponent:@"resources"];
    NSString *drvDir = [signedDir stringByAppendingPathComponent:@"drivers"];
    NSString *devconPath = [drvDir stringByAppendingPathComponent:@"devcon.exe"];
    if (![fm fileExistsAtPath:[binDir stringByAppendingPathComponent:@"appsandbox-agent.exe"]]) {
        [fm removeItemAtPath:tmp error:nil];
        emit_error(@"build-windows: signed payload zip did not yield resources/appsandbox-agent.exe");
        return 3;
    }
    emit_log(@"using downloaded signed guest payload");

    /* NetKVM (virtio-net) ships in its own small vendored zip (BSD-3-Clause), pulled + cached
     * separately because it changes independently of the signed release. Best-effort: a VM without it
     * still comes online over ivshmem (it just has no guest network). The zip is flat -- extract into
     * netkvmDir and stage from there below. */
    NSString *netkvmDir = nil;
    if (netkvmZip.length && [fm fileExistsAtPath:netkvmZip]) {
        netkvmDir = [tmp stringByAppendingPathComponent:@"netkvm"];
        (void)run_tool(@"/usr/bin/unzip", @[@"-o", @"-q", netkvmZip, @"-d", netkvmDir], NULL, NULL);
        if (![fm fileExistsAtPath:[netkvmDir stringByAppendingPathComponent:@"netkvm.sys"]]) {
            emit_log(@"netkvm zip extract failed; VM will build without a network driver");
            netkvmDir = nil;
        }
    } else {
        emit_log(@"no netkvm zip provided; VM will build without a network driver");
    }

    /* Mount the ISO + locate install.wim FIRST so the OS language can be read from the
     * WIM before the answer file is generated; the mount stays live for the build below. */
    NSString *wim = nil;
    if (!resourcesOnly) {
    emit_progress(2, @"Mounting ISO");
    NSString *err = nil;
    if (!bw_mount_iso(iso, &g_iso_mount, &err)) {
        [fm removeItemAtPath:tmp error:nil];
        emit_error(err); return 4;
    }
    wim = [g_iso_mount.mountPt stringByAppendingPathComponent:@"sources/install.wim"];
    emit_log([NSString stringWithFormat:@"install.wim: %@", wim]);

    /* OS language: unless an explicit --lang was given (lang non-empty), auto-detect the
     * install image's default UI language from the WIM XML (<IMAGE INDEX=n><WINDOWS>
     * <LANGUAGES><DEFAULT>), reading the same index we apply. Falls back to en-US. The tag
     * flows unchanged into the shared asb_provision_unattend() below. */
    if (lang.length == 0) {
        wim_t *lw = wim_open(wim.fileSystemRepresentation);
        if (lw) {
            char lbuf[32];
            if (wim_image_default_language(lw, (uint32_t)imageIdx, lbuf, sizeof lbuf) == 0 &&
                ((lbuf[0] >= 'A' && lbuf[0] <= 'Z') || (lbuf[0] >= 'a' && lbuf[0] <= 'z'))) {  /* BCP-47 starts with a letter */
                lang = @(lbuf);
                emit_lang(@(lbuf));   /* daemon surfaces this as "Detected ISO language: <tag>" in the create log */
            }
            wim_close(lw);
        }
        if (lang.length == 0) { lang = @"en-US"; emit_log(@"ISO language not detected; using en-US"); }
    }

    }

    /* Provisioning files. In the daemon flow they are generated IN-PROCESS by the host (the password
     * is encoded there via the shared asb_provision_unattend and never reaches this child) and handed
     * over via --prov-dir -- the exact mirror of the Windows backend, where vhdx_create_thread
     * generates unattend.xml/setup.cmd/SetupComplete.cmd and iso-patch.exe only consumes the --stage
     * manifest. Direct CLI use (no --prov-dir) falls back to generating them here from
     * --pass/--user/--vm-name/--lang. */
    NSString *provBase = provDir.length ? provDir : tmp;
    NSString *unattend = [provBase stringByAppendingPathComponent:@"unattend.xml"];
    NSString *setupCmd = [provBase stringByAppendingPathComponent:@"setup.cmd"];
    NSString *setupComplete = [provBase stringByAppendingPathComponent:@"SetupComplete.cmd"];
    /* SSH MSI basename: staged to \Windows\AppSandbox\<basename> in the manifest below; SetupComplete
     * (generated host-side in --prov-dir mode, or here otherwise) references it to install OpenSSH. */
    NSString *sshMsiName = (sshMsi.length && [fm fileExistsAtPath:sshMsi]) ? sshMsi.lastPathComponent : nil;
    if (provDir.length) {
        /* Host generated them; the language is still auto-detected below and reported via LANG: so the
         * host re-generates unattend.xml in place before we stage it. Just verify they exist. */
        for (NSString *p in @[unattend, setupCmd, setupComplete]) {
            if (![fm fileExistsAtPath:p]) {
                bw_unmount_iso(&g_iso_mount);
                [fm removeItemAtPath:tmp error:nil];
                emit_error([NSString stringWithFormat:@"prov-dir missing %@", p.lastPathComponent]);
                return 3;
            }
        }
    } else {
        emit_progress(3, @"Generating answer file + setup scripts");
        /* Generate the provisioning files via the SHARED generator (same source the Windows backend
         * uses -- no mac/pc fork). testMode is the caller's choice (mirrors the Windows disk builder,
         * which passes config.test_mode): when on it bakes `bcdedit /set testsigning on` so the
         * test-signed guest drivers load. arm64. */
        BOOL scriptsOK = NO;
        {
            FILE *fu = fopen(unattend.fileSystemRepresentation, "wb");
            FILE *fs = fopen(setupCmd.fileSystemRepresentation, "wb");
            FILE *fc = fopen(setupComplete.fileSystemRepresentation, "wb");
            if (fu && fs && fc) {
                if (isTemplate) asb_provision_unattend_template(fu, vmName.UTF8String, "arm64", testMode, 1);
                else asb_provision_unattend(fu, vmName.UTF8String, user.UTF8String, pass.UTF8String,
                                       "arm64", testMode, /*is_arm64=*/1, lang.UTF8String, NULL);
                asb_provision_setup_cmd(fs, 1, resourcesOnly);
                asb_provision_setupcomplete(fc, sshMsiName.length ? sshMsiName.UTF8String : NULL, resourcesOnly);
                scriptsOK = YES;
            }
            if (fu) fclose(fu);
            if (fs) fclose(fs);
            if (fc) fclose(fc);
        }
        if (!scriptsOK) {
            bw_unmount_iso(&g_iso_mount);
            [fm removeItemAtPath:tmp error:nil];
            emit_error(@"failed to write staging scripts"); return 3;
        }
    }

    /* 2) build the manifest: "<host-src>\t<guest-dest>" (mirrors generate_vhdx_manifest) */
    NSString *manifest = [tmp stringByAppendingPathComponent:@"manifest.tsv"];
    FILE *mf = fopen(manifest.fileSystemRepresentation, "w");
    if (!mf) { bw_unmount_iso(&g_iso_mount); [fm removeItemAtPath:tmp error:nil]; emit_error(@"cannot open manifest"); return 3; }
    int n = 0;
    n += bw_manifest_add(mf, unattend,      "\\Windows\\Panther\\unattend.xml");
    if (!isTemplate) {
    n += bw_manifest_add(mf, setupCmd,      "\\Windows\\AppSandbox\\setup.cmd");
    n += bw_manifest_add(mf, setupComplete, "\\Windows\\Setup\\Scripts\\SetupComplete.cmd");
    /* agent + channel helper EXEs (existence-gated -- mirrors generate_vhdx_manifest's bin set;
     * appsandbox-displays.exe is staged when present in the payload). */
    const char *bins[] = { "appsandbox-agent.exe", "appsandbox-input.exe", "appsandbox-displays.exe",
                           "appsandbox-clipboard.exe", "appsandbox-clipboard-reader.exe",
                           "appsandbox-audio.exe" };
    for (size_t i = 0; i < sizeof(bins)/sizeof(bins[0]); i++) {
        NSString *src = [binDir stringByAppendingPathComponent:@(bins[i])];
        n += bw_manifest_add(mf, src, [NSString stringWithFormat:@"\\Windows\\AppSandbox\\%s", bins[i]].UTF8String);
    }
    /* Our signed drivers (VDD + VAD + ivshmem SHM) + their certs, from the signed payload. Existence-gated. */
    const char *drv[] = { "AppSandboxVDD.inf", "AppSandboxVDD.dll", "AppSandboxVDD.cat", "AppSandboxVDD.cer",
                          "AppSandboxVAD.inf", "AppSandboxVAD.sys", "AppSandboxVAD.cat", "AppSandboxVAD.cer",
                          "AppSandboxSHM.inf", "AppSandboxSHM.sys", "AppSandboxSHM.cat", "AppSandboxSHM.cer" };
    for (size_t i = 0; i < sizeof(drv)/sizeof(drv[0]); i++) {
        NSString *src = [drvDir stringByAppendingPathComponent:@(drv[i])];
        n += bw_manifest_add(mf, src, [NSString stringWithFormat:@"\\Windows\\AppSandbox\\drivers\\%s", drv[i]].UTF8String);
    }
    /* NetKVM (virtio-net) -- vendored from virtio-win (BSD-3-Clause; license staged alongside), from its
     * own zip (netkvmDir). Mac payload only; never in the Windows-host manifest. */
    if (netkvmDir) {
        const char *nk[] = { "netkvm.inf", "netkvm.sys", "netkvm.cat", "netkvmp.exe", "virtio-win_license.txt" };
        for (size_t i = 0; i < sizeof(nk)/sizeof(nk[0]); i++) {
            NSString *src = [netkvmDir stringByAppendingPathComponent:@(nk[i])];
            n += bw_manifest_add(mf, src, [NSString stringWithFormat:@"\\Windows\\AppSandbox\\drivers\\%s", nk[i]].UTF8String);
        }
    }
    /* devcon.exe (ARM64) -- installs the root-enumerated VDD/VAD devnodes */
    if (devconPath) n += bw_manifest_add(mf, devconPath, "\\Windows\\AppSandbox\\drivers\\devcon.exe");
    /* OpenSSH MSI (downloaded at create) -- SetupComplete.cmd installs it for SSH-over-ivshmem ch7 */
    if (sshMsiName)
        n += bw_manifest_add(mf, sshMsi, [NSString stringWithFormat:@"\\Windows\\AppSandbox\\%@", sshMsiName].UTF8String);
    }
    fclose(mf);
    emit_log([NSString stringWithFormat:@"manifest: %d file(s) to stage", n]);

    if (resourcesOnly) {
        NSError *mediaError = nil;
        NSString *media = [tmp stringByAppendingPathComponent:@"media"];
        [fm createDirectoryAtPath:media withIntermediateDirectories:YES attributes:nil error:&mediaError];
        NSString *entries = [NSString stringWithContentsOfFile:manifest encoding:NSUTF8StringEncoding error:&mediaError];
        BOOL copied = entries != nil;
        for (NSString *line in [entries componentsSeparatedByString:@"\n"]) {
            if (!line.length) continue;
            NSArray *pair = [line componentsSeparatedByString:@"\t"];
            if (pair.count != 2) { copied = NO; break; }
            NSString *guest = [pair[1] stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
            NSString *relative;
            if ([guest isEqualToString:@"/Windows/Panther/unattend.xml"]) relative = @"unattend.xml";
            else if ([guest isEqualToString:@"/Windows/AppSandbox/setup.cmd"]) relative = @"setup.cmd";
            else if ([guest isEqualToString:@"/Windows/Setup/Scripts/SetupComplete.cmd"]) relative = @"SetupComplete.cmd";
            else if ([guest hasPrefix:@"/Windows/AppSandbox/"])
                relative = [guest substringFromIndex:20];
            else { copied = NO; break; }
            NSString *destination = [media stringByAppendingPathComponent:relative];
            [fm createDirectoryAtPath:destination.stringByDeletingLastPathComponent
                 withIntermediateDirectories:YES attributes:nil error:&mediaError];
            if (![fm copyItemAtPath:pair[0] toPath:destination error:&mediaError]) { copied = NO; break; }
        }
        NSString *isoError = nil;
        int result = copied ? run_tool(@"/usr/bin/hdiutil", @[@"makehybrid", @"-iso", @"-joliet",
            @"-default-volume-name", @"APPSETUP", @"-o", out, media], NULL, &isoError) : -1;
        [fm removeItemAtPath:tmp error:nil];
        if (result != 0) {
            NSString *detail = mediaError.localizedDescription ?: isoError ?: @"Invalid staging manifest";
            emit_error([NSString stringWithFormat:@"Failed to build instance resources ISO: %@",
                [detail stringByReplacingOccurrencesOfString:@"\n" withString:@" "]]);
            return 5;
        }
        emit_done(out);
        return 0;
    }

    /* build the disk (apply WIM image -> NTFS -> stage our files -> ESP + BCD); the ISO is
     * already mounted + install.wim located above (for language detection). Use LOG (not
     * STATUS) so the apply monitor's percentage isn't reset to the indeterminate sentinel. */
    emit_log(@"Building Windows disk");
    int rc = win_disk_build(wim.fileSystemRepresentation,
                            out.fileSystemRepresentation,
                            (uint32_t)imageIdx,
                            (uint64_t)(diskGb > 0 ? diskGb : 64),
                            manifest.fileSystemRepresentation,
                            bw_progress);
    check_cancelled();

    /* 5) unmount + cleanup */
    bw_unmount_iso(&g_iso_mount);
    [fm removeItemAtPath:tmp error:nil];

    if (rc != 0) { emit_error([NSString stringWithFormat:@"win_disk_build failed (%d)", rc]); return 5; }
    emit_progress(100, @"Disk complete");
    emit_done(out);
    return 0;
}

/* ---- Entry ---- */

static void print_usage(void) {
    fprintf(stderr,
        "Usage: iso-patch-mac <subcommand> [args]\n"
        "\n"
        "Subcommands:\n"
        "  fetch-ipsw --output <path>\n"
        "      Download the latest supported macOS restore image.\n"
        "\n"
        "  install --name <vm> --vm-dir <dir> --ipsw <path>\n"
        "          --ram-mb <n> --cpus <n> --disk-gb <n>\n"
        "      Create disk image + aux storage + hardware model +\n"
        "      machine identifier, then run VZMacOSInstaller.\n"
        "\n"
        "  stage --disk <path> --manifest <path>\n"
        "      Mount the VM disk image, apply TSV manifest, unmount.\n"
        "      Must run through the elevated AppSandbox core.\n"
        "\n"
        "  build-windows --iso <path> --out <disk.img> --signed-payload-zip <zip> [--netkvm-zip <zip>]\n"
        "                [--devcon <devcon.exe>] [--ssh-msi <OpenSSH .msi>] [--vm-name <n>]\n"
        "                [--user <u>] [--pass <p>] [--prov-dir <dir>] [--lang <bcp47>] [--disk-gb <n>] [--image-index <n>]\n"
        "      --prov-dir supplies host-generated unattend.xml/setup.cmd/SetupComplete.cmd (password\n"
        "      encoded host-side); without it they are generated here from --user/--pass/--vm-name.\n"
        "      From-scratch Windows VM disk: mount the ISO, apply install.wim with our\n"
        "      own NTFS writer, and stage the answer file + agent + test-signed drivers.\n"
        "\n");
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        if (geteuid() == 0 && setuid(0) != 0) {
            emit_error(@"Failed to initialize worker process credentials");
            return 1;
        }
        if (argc < 2) { print_usage(); return 64; }
        if (getpgrp() != getpid() && setpgid(0, 0) != 0) {
            emit_error(@"Failed to initialize worker process group");
            return 1;
        }
        const char *cmd = argv[1];
        if (strcmp(cmd, "stage") == 0 || strcmp(cmd, "build-windows") == 0) {
            atexit(cleanup_worker);
            struct sigaction action = {0};
            action.sa_handler = cancel_on_signal;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESTART;
            sigaction(SIGTERM, &action, NULL);
            sigaction(SIGINT, &action, NULL);
            signal(SIGPIPE, SIG_IGN);
        }
        if (strcmp(cmd, "stage") == 0) {
            return cmd_stage(argc - 2, argv + 2);
        }
        if (strcmp(cmd, "fetch-ipsw") == 0) {
            return cmd_fetch_ipsw(argc - 2, argv + 2);
        }
        if (strcmp(cmd, "install") == 0) {
            return cmd_install(argc - 2, argv + 2);
        }
        if (strcmp(cmd, "build-windows") == 0) {
            return cmd_build_windows(argc - 2, argv + 2);
        }
        if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
            print_usage();
            return 0;
        }
        fprintf(stderr, "unknown subcommand: %s\n", cmd);
        print_usage();
        return 64;
    }
}
