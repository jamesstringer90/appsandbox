#import "vm_dir.h"

@implementation VmDir

+ (NSURL *)vmsRootDirectory {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSURL *appSupport = [fm URLForDirectory:NSApplicationSupportDirectory
                                    inDomain:NSUserDomainMask
                           appropriateForURL:nil
                                      create:YES
                                       error:nil];
    NSURL *root = [[appSupport URLByAppendingPathComponent:@"AppSandbox" isDirectory:YES]
                      URLByAppendingPathComponent:@"VMs" isDirectory:YES];
    [fm createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];
    return root;
}

+ (NSURL *)directoryForVm:(NSString *)name {
    return [[self vmsRootDirectory] URLByAppendingPathComponent:name isDirectory:YES];
}

+ (NSURL *)diskImageURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"disk.img"];
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
    return [[NSFileManager defaultManager] createDirectoryAtURL:dir
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:error];
}

+ (BOOL)vmExists:(NSString *)name {
    BOOL isDir = NO;
    NSString *path = [self directoryForVm:name].path;
    return [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDir] && isDir;
}

+ (BOOL)deleteVm:(NSString *)name error:(NSError **)error {
    NSURL *dir = [self directoryForVm:name];
    return [[NSFileManager defaultManager] removeItemAtURL:dir error:error];
}

@end
