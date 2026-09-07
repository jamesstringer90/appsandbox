#import "vm_dir.h"
#import "asb_core_mac.h"

#include <pwd.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

@implementation VmDir

+ (NSURL *)vmsRootDirectory {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSURL *appSupport = nil;

    /* Under sudo (the headless daemon's required launch mode), NSUserDomainMask
     * resolves to root's home (/var/root) -- a DIFFERENT VM registry and a
     * cold restore-image cache than the user's GUI. Resolve the INVOKING
     * user's real home instead (SUDO_USER -> passwd), so `sudo AppSandbox
     * --headless` manages the same VMs the user sees in the app. Dormant in
     * the GUI (never runs as root). */
    const char *sudo_user = getenv("SUDO_USER");
    if (geteuid() == 0 && sudo_user && *sudo_user) {
        struct passwd *pw = getpwnam(sudo_user);
        if (pw && pw->pw_dir && pw->pw_dir[0]) {
            appSupport = [NSURL fileURLWithPath:
                [[NSString stringWithUTF8String:pw->pw_dir]
                    stringByAppendingPathComponent:@"Library/Application Support"]
                                    isDirectory:YES];
        }
    }
    if (!appSupport) {
        appSupport = [fm URLForDirectory:NSApplicationSupportDirectory
                                inDomain:NSUserDomainMask
                       appropriateForURL:nil
                                  create:YES
                                   error:nil];
    }
    NSURL *root = [[appSupport URLByAppendingPathComponent:@"AppSandbox" isDirectory:YES]
                      URLByAppendingPathComponent:@"VMs" isDirectory:YES];
    [fm createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];
    return root;
}

+ (NSURL *)directoryForVm:(NSString *)name {
    return [[self vmsRootDirectory] URLByAppendingPathComponent:name isDirectory:YES];
}

+ (NSURL *)diskImageURLFor:(NSString *)name {
    return [[self diskDirectoryForVm:name] URLByAppendingPathComponent:@"disk.img"];
}

+ (NSString *)normalizedDiskDirectory:(NSString *)directory {
    if (!directory.length) return @"";
    NSString *path = directory.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    NSString *root = [self vmsRootDirectory].path;
    struct stat selected, original;
    if ([path isEqualToString:root] ||
        (stat(path.fileSystemRepresentation, &selected) == 0 &&
         stat(root.fileSystemRepresentation, &original) == 0 &&
         selected.st_dev == original.st_dev && selected.st_ino == original.st_ino))
        return @"";
    return path;
}

+ (NSString *)validationErrorForDiskDirectory:(NSString *)directory vmName:(NSString *)name {
    if (!directory.length) return nil;
    if ([directory rangeOfCharacterFromSet:[NSCharacterSet controlCharacterSet]].location != NSNotFound)
        return @"Disk folder cannot contain control characters.";
    if (![directory hasPrefix:@"/"])
        return @"Disk folder must be an absolute path.";
    NSString *parent = [self normalizedDiskDirectory:directory];
    if (!parent.length) return nil;
    NSString *child = [parent stringByAppendingPathComponent:name];
    if ([directory lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= 1024 ||
        [[child stringByAppendingPathComponent:@"disk.img"] lengthOfBytesUsingEncoding:NSUTF8StringEncoding] >= 1024)
        return @"Disk folder path is too long.";
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDirectory = NO;
    if (![fm fileExistsAtPath:parent isDirectory:&isDirectory] || !isDirectory)
        return @"Disk folder must be an existing folder.";
    if (![fm isWritableFileAtPath:parent])
        return @"Disk folder is not writable.";
    struct stat existing;
    if (lstat(child.fileSystemRepresentation, &existing) == 0)
        return @"A folder or file for this VM already exists in the disk storage location.";
    return nil;
}

+ (NSURL *)diskDirectoryForVm:(NSString *)name {
    AsbVmMac *vm = asb_mac_vm_find(name.UTF8String);
    if (vm && vm->disk_directory[0])
        return [[NSURL fileURLWithPath:@(vm->disk_directory) isDirectory:YES]
                    URLByAppendingPathComponent:name isDirectory:YES];
    return [self directoryForVm:name];
}

+ (NSURL *)auxiliaryStorageURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"aux.img"];
}

+ (NSURL *)hardwareModelURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"hardware.bin"];
}

+ (NSURL *)machineIdentifierURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"machine-id.bin"];
}

+ (BOOL)ensureDirectoryFor:(NSString *)name error:(NSError **)error {
    NSURL *dir = [self directoryForVm:name];
    NSURL *diskDir = [self diskDirectoryForVm:name];
    BOOL external = ![diskDir isEqual:dir];
    /* A missing external parent must not be recreated as local storage. */
    if (external && mkdir(diskDir.fileSystemRepresentation, 0755) != 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return NO;
    }
    BOOL ok = [[NSFileManager defaultManager] createDirectoryAtURL:dir
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:error];
    if (!ok && external) rmdir(diskDir.fileSystemRepresentation);
    return ok;
}

+ (BOOL)vmExists:(NSString *)name {
    BOOL isDir = NO;
    NSString *path = [self directoryForVm:name].path;
    return [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] && isDir;
}

+ (BOOL)deleteVm:(NSString *)name error:(NSError **)error {
    NSURL *dir = [self directoryForVm:name];
    NSURL *diskDir = [self diskDirectoryForVm:name];
    if (![diskDir isEqual:dir]) {
        NSFileManager *fm = [NSFileManager defaultManager];
        /* An offline volume must not silently orphan its VM disk. */
        BOOL isDirectory = NO;
        if (![fm fileExistsAtPath:diskDir.URLByDeletingLastPathComponent.path isDirectory:&isDirectory] || !isDirectory) {
            if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOENT userInfo:nil];
            return NO;
        }
        NSURL *disk = [self diskImageURLFor:name];
        if ([fm fileExistsAtPath:disk.path] && ![fm removeItemAtURL:disk error:error]) return NO;
        /* Leave unrelated files in the VM's disk directory. */
        rmdir(diskDir.fileSystemRepresentation);
    }
    return [[NSFileManager defaultManager] removeItemAtURL:dir error:error];
}

@end
