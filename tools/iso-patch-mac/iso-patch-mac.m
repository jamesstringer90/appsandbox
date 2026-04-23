/*
 * iso-patch-mac -- Host-side CLI for preparing a macOS VM disk image.
 *
 * Mirrors tools/iso-patch/iso-patch.c on Windows. Responsibilities:
 *   - "stage": mount the APFS Data volume of a VZ disk image, copy files
 *     per a TSV manifest, fix ownership/permissions, then detach.
 *   - IPSW fetch + VZMacOSInstaller orchestration (analogue of
 *     iso-patch's --to-vhdx mode).
 *
 * Privilege model: invoked by the host via AuthorizationExecuteWithPrivileges,
 * so the tool runs as root. It does not escalate itself.
 *
 * Stdout protocol (read by src/backend_mac/iso_patch_mac.m):
 *   STATUS:<text>
 *   PROGRESS:<percent>:<step>
 *   LOG:<line>
 *   DONE:<disk path>
 *   ERROR:<reason>
 *
 * Manifest TSV (one file per line, tab-separated, "#" comments ignored):
 *   <host-source-path>\t<dest-relative-to-Data-volume>\t<mode-octal>\t<owner>
 * Example:
 *   /opt/agent/appsandbox-agent\tLibrary/AppSandbox/appsandbox-agent\t0755\troot:wheel
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <CommonCrypto/CommonKeyDerivation.h>

/* ---- stdout protocol helpers ---- */

static void emit(NSString *prefix, NSString *msg) {
    fprintf(stdout, "%s:%s\n", prefix.UTF8String, msg.UTF8String);
    fflush(stdout);
}
static void emit_status(NSString *msg)   { emit(@"STATUS",   msg); }
static void emit_progress(int pct, NSString *step) {
    fprintf(stdout, "PROGRESS:%d:%s\n", pct, step.UTF8String);
    fflush(stdout);
}
static void emit_log(NSString *msg)      { emit(@"LOG",      msg); }
static void emit_done(NSString *path)    { emit(@"DONE",     path); }
static void emit_error(NSString *msg)    { emit(@"ERROR",    msg); }

/* ---- Subprocess runner ---- */

static int run_tool(NSString *launchPath, NSArray<NSString *> *args,
                    NSString **stdoutStr, NSString **stderrStr) {
    NSTask *task = [[NSTask alloc] init];
    task.launchPath = launchPath;
    task.arguments = args ?: @[];
    NSPipe *outPipe = [NSPipe pipe];
    NSPipe *errPipe = [NSPipe pipe];
    task.standardOutput = outPipe;
    task.standardError  = errPipe;

    NSError *err = nil;
    if (![task launchAndReturnError:&err]) {
        if (stderrStr) *stderrStr = err.localizedDescription ?: @"launch failed";
        return -1;
    }
    NSData *outD = [outPipe.fileHandleForReading readDataToEndOfFile];
    NSData *errD = [errPipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    if (stdoutStr)
        *stdoutStr = [[NSString alloc] initWithData:outD encoding:NSUTF8StringEncoding];
    if (stderrStr)
        *stderrStr = [[NSString alloc] initWithData:errD encoding:NSUTF8StringEncoding];
    return task.terminationStatus;
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
    NSString *src;
    NSString *destRel;
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

/* ---- User injection (dslocal) ---- */

static NSData *shadow_hash_data_for_password(const char *pw, size_t pwLen) {
    unsigned char salt[32];
    arc4random_buf(salt, 32);
    const int iterations = 150000;
    unsigned char entropy[128];
    int rc = CCKeyDerivationPBKDF(kCCPBKDF2, pw, pwLen,
                                   salt, sizeof(salt),
                                   kCCPRFHmacAlgSHA512,
                                   iterations,
                                   entropy, sizeof(entropy));
    if (rc != 0) return nil;

    NSDictionary *inner = @{
        @"SALTED-SHA512-PBKDF2": @{
            @"entropy":    [NSData dataWithBytes:entropy length:sizeof(entropy)],
            @"iterations": @(iterations),
            @"salt":       [NSData dataWithBytes:salt length:sizeof(salt)],
        }
    };
    NSData *plist = [NSPropertyListSerialization dataWithPropertyList:inner
                                                                format:NSPropertyListBinaryFormat_v1_0
                                                               options:0
                                                                 error:NULL];
    memset(entropy, 0, sizeof(entropy));
    memset(salt,    0, sizeof(salt));
    return plist;
}

/* macOS autologin password obfuscation (/etc/kcpassword). */
static NSData *kcpassword_for(const char *pw, size_t pwLen) {
    static const unsigned char cipher[] = {
        0x7D, 0x89, 0x52, 0x23, 0xD2, 0xBC, 0xDD, 0xEA, 0xA3, 0xB9, 0x1F
    };
    size_t outLen = ((pwLen / 12) + 1) * 12; /* round up to next multiple of 12 */
    unsigned char *buf = malloc(outLen);
    for (size_t i = 0; i < outLen; i++) {
        unsigned char src = (i < pwLen) ? (unsigned char)pw[i] : cipher[i % 11];
        buf[i] = src ^ cipher[i % 11];
    }
    NSData *result = [NSData dataWithBytes:buf length:outLen];
    memset(buf, 0, outLen);
    free(buf);
    return result;
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

static BOOL add_user_to_admin_group(NSString *mountPt, NSString *shortname,
                                     NSString *userUUID, NSString **errOut) {
    NSString *adminPath = [mountPt stringByAppendingPathComponent:
        @"private/var/db/dslocal/nodes/Default/groups/admin.plist"];
    NSMutableDictionary *admin = [[NSDictionary dictionaryWithContentsOfFile:adminPath]
                                   mutableCopy];
    if (!admin) {
        /* Some macOS installs put it under /var/db/... Try both. */
        adminPath = [mountPt stringByAppendingPathComponent:
            @"var/db/dslocal/nodes/Default/groups/admin.plist"];
        admin = [[NSDictionary dictionaryWithContentsOfFile:adminPath] mutableCopy];
    }
    if (!admin) {
        if (errOut) *errOut = @"admin group plist not found on disk";
        return NO;
    }

    NSMutableArray *users   = [(admin[@"users"]        ?: @[]) mutableCopy];
    NSMutableArray *members = [(admin[@"groupmembers"] ?: @[]) mutableCopy];
    if (![users   containsObject:shortname]) [users   addObject:shortname];
    if (![members containsObject:userUUID])  [members addObject:userUUID];
    admin[@"users"]        = users;
    admin[@"groupmembers"] = members;

    return write_plist(admin, adminPath, 0, 80, 0600, errOut);
}

static BOOL inject_user(NSString *mountPt,
                        NSString *shortname, NSString *realname, int uid,
                        const char *password, size_t passwordLen,
                        NSString **errOut) {
    emit_status([NSString stringWithFormat:@"Creating user '%@'", shortname]);

    NSData *shd = shadow_hash_data_for_password(password, passwordLen);
    if (!shd) {
        if (errOut) *errOut = @"PBKDF2 failed";
        return NO;
    }

    NSString *uuidStr = [[NSUUID UUID] UUIDString];
    NSDictionary *userPlist = @{
        @"authentication_authority":
            @[@";ShadowHash;HASHLIST:<SALTED-SHA512-PBKDF2>"],
        @"generateduid": @[uuidStr],
        @"gid":          @[@"20"],
        @"home":         @[[NSString stringWithFormat:@"/Users/%@", shortname]],
        @"name":         @[shortname],
        @"passwd":       @[@"********"],
        @"realname":     @[realname],
        @"shell":        @[@"/bin/zsh"],
        @"uid":          @[[NSString stringWithFormat:@"%d", uid]],
        @"ShadowHashData": @[shd],
    };

    NSString *userPlistPath = [mountPt stringByAppendingPathComponent:
        [NSString stringWithFormat:
         @"private/var/db/dslocal/nodes/Default/users/%@.plist", shortname]];
    if (!write_plist(userPlist, userPlistPath, 0, 0, 0600, errOut)) {
        /* Fall back to non-firmlinked path if private/ layout differs. */
        userPlistPath = [mountPt stringByAppendingPathComponent:
            [NSString stringWithFormat:
             @"var/db/dslocal/nodes/Default/users/%@.plist", shortname]];
        if (!write_plist(userPlist, userPlistPath, 0, 0, 0600, errOut)) return NO;
    }

    if (!add_user_to_admin_group(mountPt, shortname, uuidStr, errOut)) return NO;

    /* Pre-create home dir skeleton so per-user SetupAssistant.plist persists. */
    NSString *homePath = [mountPt stringByAppendingPathComponent:
        [NSString stringWithFormat:@"Users/%@", shortname]];
    NSString *prefsPath = [homePath stringByAppendingPathComponent:@"Library/Preferences"];
    [[NSFileManager defaultManager] createDirectoryAtPath:prefsPath
                                withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
    /* Recursively chown home → uid:staff (gid 20). */
    NSTask *t = [[NSTask alloc] init];
    t.launchPath = @"/usr/sbin/chown";
    t.arguments = @[@"-R",
                    [NSString stringWithFormat:@"%d:20", uid],
                    homePath];
    [t launchAndReturnError:nil];
    [t waitUntilExit];
    chmod(homePath.fileSystemRepresentation, 0700);

    return YES;
}

/* Create an empty file with given owner/mode; mkdir -p'ing the parent. */
static BOOL touch_marker(NSString *path, uid_t uid, gid_t gid, mode_t mode,
                         NSString **errOut) {
    [[NSFileManager defaultManager] createDirectoryAtPath:
        [path stringByDeletingLastPathComponent]
                                withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
    int fd = open(path.fileSystemRepresentation, O_CREAT|O_WRONLY|O_TRUNC, mode);
    if (fd < 0) {
        if (errOut) *errOut = [NSString stringWithFormat:@"touch %@: %s",
                               path.lastPathComponent, strerror(errno)];
        return NO;
    }
    close(fd);
    chown(path.fileSystemRepresentation, uid, gid);
    chmod(path.fileSystemRepresentation, mode);
    return YES;
}

static BOOL skip_setup_assistant(NSString *mountPt, NSString *shortname, int uid,
                                 NSString **errOut) {
    emit_status(@"Skipping Setup Assistant");

    /* ---- System-wide marker files ----
     * Four flags together cover all the code paths inside the OS that
     * decide whether to run the initial setup Buddy:
     *   .AppleSetupDone       — the main system-level flag
     *   .SetupRegComplete     — older receipt path, still checked
     *   User Template/.skipbuddy — suppresses Buddy for future logins too
     */
    NSString *doneFile = [mountPt stringByAppendingPathComponent:
        @"private/var/db/.AppleSetupDone"];
    if (!touch_marker(doneFile, 0, 0, 0644, errOut)) {
        doneFile = [mountPt stringByAppendingPathComponent:@"var/db/.AppleSetupDone"];
        if (!touch_marker(doneFile, 0, 0, 0644, errOut)) return NO;
    }

    (void)touch_marker([mountPt stringByAppendingPathComponent:
        @"Library/Receipts/.SetupRegComplete"], 0, 0, 0644, NULL);
    (void)touch_marker([mountPt stringByAppendingPathComponent:
        @"Library/User Template/English.lproj/.skipbuddy"], 0, 0, 0644, NULL);
    (void)touch_marker([mountPt stringByAppendingPathComponent:
        @"Library/User Template/Non_localized.lproj/.skipbuddy"], 0, 0, 0644, NULL);

    /* Per-user com.apple.SetupAssistant.plist — DidSee* flags + a few
     * MiniBuddy gate keys. On non-MDM Tahoe, this suppresses the older
     * panes (Privacy, Siri, Screen Time, Touch ID, True Tone, Appearance,
     * Accessibility, etc.). It does NOT suppress Apple Account, Age Range,
     * Analytics, FileVault, or the Software Update prompt — those panes
     * only respect the com.apple.SetupAssistant.managed domain via real
     * MDM enrollment, which is out of scope here. The user clicks through
     * those 5 once on first VM creation. */
    NSDictionary *sa = @{
        @"DidSeeAccessibility":                 @YES,
        @"DidSeeAccessibilitySetup":            @YES,
        @"DidSeeActivationLock":                @YES,
        @"DidSeeAppearanceSetup":               @YES,
        @"DidSeeAppStore":                      @YES,
        @"DidSeeAvatarSetup":                   @YES,
        @"DidSeeContinuity":                    @YES,
        @"DidSeeEmergencySOS":                  @YES,
        @"DidSeeIntelligence":                  @YES,
        @"DidSeeLockdownMode":                  @YES,
        @"DidSeeOSShowcase":                    @YES,
        @"DidSeePrivacy":                       @YES,
        @"DidSeePrivacyConsent":                @YES,
        @"DidSeeScreenTime":                    @YES,
        @"DidSeeSiriSetup":                     @YES,
        @"DidSeeTerms":                         @YES,
        @"DidSeeTermsOfAddress":                @YES,
        @"DidSeeTOS":                           @YES,
        @"DidSeeTouchIDSetup":                  @YES,
        @"DidSeeTrueToneDisplay":               @YES,
        @"DidSeeUpdateCompleted":               @YES,

        /* Gate keys MiniBuddy consults alongside the DidSee flags. Setting
         * these explicitly prevents unrelated "you haven't finished setup"
         * prompts on subsequent boots. */
        @"InitialAccountOnMac":                 @YES,
        @"MiniBuddyLaunchReason":               @0,
        @"MiniBuddyLaunchedPostMigration":      @YES,
        @"MiniBuddyShouldLaunchToResumeSetup":  @NO,

        @"SkipExpressSettingsUpdating":         @YES,
        @"SkipFirstLoginOptimization":          @YES,
        @"SkipFirstLoginOptIn":                 @YES,

        @"GestureMovieSeen":                    @"none",
        /* Out-of-range sentinel: prevents re-prompts after OS updates. */
        @"LastSeenBuddyBuildVersion":           @"99.99",
        @"LastSeenCloudProductVersion":         @"99.99",
        @"PreviousBuildVersion":                @"99.99",
        @"PreviousSystemVersion":               @"99.99",
        @"RunNonInteractive":                   @YES,
    };
    NSString *saUserPath = [mountPt stringByAppendingPathComponent:
        [NSString stringWithFormat:
         @"Users/%@/Library/Preferences/com.apple.SetupAssistant.plist", shortname]];
    if (!write_plist(sa, saUserPath, uid, 20, 0600, errOut)) return NO;

    /* System-wide copy of the same plist — some code paths check the
     * machine-scope domain in addition to the per-user one. */
    NSString *saSysPath = [mountPt stringByAppendingPathComponent:
        @"Library/Preferences/com.apple.SetupAssistant.plist"];
    if (!write_plist(sa, saSysPath, 0, 0, 0644, errOut)) return NO;

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

static BOOL enable_autologin(NSString *mountPt, NSString *shortname,
                             const char *password, size_t passwordLen,
                             NSString **errOut) {
    emit_status(@"Enabling auto-login");

    /* /Library/Preferences/com.apple.loginwindow.plist: autoLoginUser. */
    NSString *lwPath = [mountPt stringByAppendingPathComponent:
        @"Library/Preferences/com.apple.loginwindow.plist"];
    NSMutableDictionary *lw =
        [[NSDictionary dictionaryWithContentsOfFile:lwPath] mutableCopy]
            ?: [NSMutableDictionary dictionary];
    lw[@"autoLoginUser"] = shortname;
    if (!write_plist(lw, lwPath, 0, 0, 0644, errOut)) return NO;

    /* /etc/kcpassword: obfuscated password bytes (XOR cipher). */
    NSData *kc = kcpassword_for(password, passwordLen);
    NSString *kcPath = [mountPt stringByAppendingPathComponent:@"private/etc/kcpassword"];
    [[NSFileManager defaultManager] createDirectoryAtPath:
        [kcPath stringByDeletingLastPathComponent]
                                withIntermediateDirectories:YES
                                                 attributes:nil error:nil];
    if (![kc writeToFile:kcPath atomically:YES]) {
        kcPath = [mountPt stringByAppendingPathComponent:@"etc/kcpassword"];
        if (![kc writeToFile:kcPath atomically:YES]) {
            if (errOut) *errOut = @"kcpassword write failed";
            return NO;
        }
    }
    chown(kcPath.fileSystemRepresentation, 0, 0);
    chmod(kcPath.fileSystemRepresentation, 0600);
    return YES;
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

/* ---- Global state for signal-safe cleanup ---- */

static MountState g_mount = {0};
static volatile sig_atomic_t g_cleaning_up = 0;

static void cleanup_on_signal(int sig) {
    (void)sig;
    if (g_cleaning_up) _exit(130);
    g_cleaning_up = 1;
    unmount_and_detach(&g_mount);
    _exit(130);
}

/* ---- "stage" subcommand ---- */

static int cmd_stage(int argc, char **argv) {
    NSString *diskPath = nil;
    NSString *manifest = nil;
    NSString *userShort = nil;
    NSString *userReal  = nil;
    int userUid = 501;
    NSString *passFile = nil;
    NSString *computerName = nil;
    BOOL skipSA = NO;
    BOOL autoLogin = NO;
    BOOL sshOn = NO;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc)
            diskPath = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc)
            manifest = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--user-shortname") == 0 && i + 1 < argc)
            userShort = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--user-realname") == 0 && i + 1 < argc)
            userReal = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--user-uid") == 0 && i + 1 < argc)
            userUid = atoi(argv[++i]);
        else if (strcmp(argv[i], "--user-password-file") == 0 && i + 1 < argc)
            passFile = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--computer-name") == 0 && i + 1 < argc)
            computerName = [NSString stringWithUTF8String:argv[++i]];
        else if (strcmp(argv[i], "--skip-setup-assistant") == 0)
            skipSA = YES;
        else if (strcmp(argv[i], "--auto-login") == 0)
            autoLogin = YES;
        else if (strcmp(argv[i], "--enable-ssh") == 0)
            sshOn = YES;
    }
    if (!diskPath || !manifest) {
        emit_error(@"stage: --disk and --manifest required");
        return 2;
    }
    if (geteuid() != 0) {
        emit_error(@"stage: must run as root (invoke via AEWP)");
        return 2;
    }

    NSString *err = nil;
    NSArray *entries = load_manifest(manifest, &err);
    if (!entries) { emit_error(err); return 3; }

    /* Read password file into a buffer we'll wipe before exit. */
    char passBuf[256] = {0};
    size_t passLen = 0;
    if (passFile) {
        int pfd = open(passFile.fileSystemRepresentation, O_RDONLY);
        if (pfd < 0) {
            emit_error([NSString stringWithFormat:@"open password file: %s", strerror(errno)]);
            return 3;
        }
        ssize_t n = read(pfd, passBuf, sizeof(passBuf) - 1);
        close(pfd);
        if (n > 0) passLen = (size_t)n;
    }

    emit_progress(5, @"Loaded manifest");

    if (!mount_disk(diskPath, &g_mount, &err)) {
        memset(passBuf, 0, sizeof(passBuf));
        emit_error(err);
        return 4;
    }

    signal(SIGINT,  cleanup_on_signal);
    signal(SIGTERM, cleanup_on_signal);
    signal(SIGPIPE, SIG_IGN);

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
            memset(passBuf, 0, sizeof(passBuf));
            emit_error(err);
            return 5;
        }
    }

    /* Optional: create user, skip Setup Assistant, enable autologin. */
    if (userShort && passLen > 0) {
        emit_progress(75, @"Creating user account");
        if (!inject_user(g_mount.mountPt, userShort, userReal ?: userShort,
                         userUid, passBuf, passLen, &err)) {
            unmount_and_detach(&g_mount);
            memset(passBuf, 0, sizeof(passBuf));
            emit_error(err);
            return 6;
        }
    }
    if (skipSA && userShort) {
        emit_progress(85, @"Skipping Setup Assistant");
        if (!skip_setup_assistant(g_mount.mountPt, userShort, userUid, &err)) {
            unmount_and_detach(&g_mount);
            memset(passBuf, 0, sizeof(passBuf));
            emit_error(err);
            return 7;
        }
    }
    if (autoLogin && userShort && passLen > 0) {
        emit_progress(90, @"Enabling auto-login");
        if (!enable_autologin(g_mount.mountPt, userShort, passBuf, passLen, &err)) {
            unmount_and_detach(&g_mount);
            memset(passBuf, 0, sizeof(passBuf));
            emit_error(err);
            return 8;
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
            memset(passBuf, 0, sizeof(passBuf));
            emit_error(err);
            return 9;
        }
    }

    if (computerName.length) {
        emit_progress(94, @"Setting computer name");
        if (!set_computer_name(g_mount.mountPt, computerName, &err)) {
            unmount_and_detach(&g_mount);
            memset(passBuf, 0, sizeof(passBuf));
            emit_error(err);
            return 10;
        }
    }

    emit_progress(95, @"Unmounting");
    unmount_and_detach(&g_mount);

    /* Wipe plaintext password from RAM before we exit. */
    memset(passBuf, 0, sizeof(passBuf));

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
    if (n == 0 || raw.length < 8 + n) return nil;
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
    NSString *name = nil, *vmDir = nil, *ipsw = nil;
    int ramMb = 0, cpus = 0, diskGb = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = @(argv[++i]);
        else if (strcmp(argv[i], "--vm-dir") == 0 && i + 1 < argc) vmDir = @(argv[++i]);
        else if (strcmp(argv[i], "--ipsw") == 0 && i + 1 < argc) ipsw = @(argv[++i]);
        else if (strcmp(argv[i], "--ram-mb") == 0 && i + 1 < argc) ramMb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cpus") == 0 && i + 1 < argc) cpus = atoi(argv[++i]);
        else if (strcmp(argv[i], "--disk-gb") == 0 && i + 1 < argc) diskGb = atoi(argv[++i]);
    }
    if (!name || !vmDir || !ipsw || ramMb <= 0 || cpus <= 0 || diskGb <= 0) {
        emit_error(@"install: --name --vm-dir --ipsw --ram-mb --cpus --disk-gb all required");
        return 2;
    }

    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *mkErr = nil;
    if (![fm createDirectoryAtPath:vmDir withIntermediateDirectories:YES
                        attributes:nil error:&mkErr]) {
        emit_error([NSString stringWithFormat:@"mkdir %@: %@", vmDir, mkErr.localizedDescription]);
        return 3;
    }

    NSString *diskPath = [vmDir stringByAppendingPathComponent:@"disk.img"];
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

                VZNATNetworkDeviceAttachment *natAtt = [[VZNATNetworkDeviceAttachment alloc] init];
                VZVirtioNetworkDeviceConfiguration *netCfg = [[VZVirtioNetworkDeviceConfiguration alloc] init];
                netCfg.attachment = natAtt;
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
        "      Must run as root (invoked via AEWP).\n"
        "\n");
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        if (argc < 2) { print_usage(); return 64; }
        const char *cmd = argv[1];
        if (strcmp(cmd, "stage") == 0) {
            return cmd_stage(argc - 2, argv + 2);
        }
        if (strcmp(cmd, "fetch-ipsw") == 0) {
            return cmd_fetch_ipsw(argc - 2, argv + 2);
        }
        if (strcmp(cmd, "install") == 0) {
            return cmd_install(argc - 2, argv + 2);
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
