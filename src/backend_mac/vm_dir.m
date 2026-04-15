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

+ (NSURL *)configURLFor:(NSString *)name {
    return [[self directoryForVm:name] URLByAppendingPathComponent:@"config.json"];
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

+ (NSArray<NSString *> *)listAllVmNames {
    NSURL *root = [self vmsRootDirectory];
    NSArray<NSURL *> *contents = [[NSFileManager defaultManager]
        contentsOfDirectoryAtURL:root
      includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                         options:NSDirectoryEnumerationSkipsHiddenFiles
                           error:nil];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (NSURL *url in contents) {
        NSNumber *isDir = nil;
        [url getResourceValue:&isDir forKey:NSURLIsDirectoryKey error:nil];
        if ([isDir boolValue]) {
            [names addObject:[url lastPathComponent]];
        }
    }
    [names sortUsingSelector:@selector(caseInsensitiveCompare:)];
    return names;
}

+ (BOOL)deleteVm:(NSString *)name error:(NSError **)error {
    NSURL *dir = [self directoryForVm:name];
    return [[NSFileManager defaultManager] removeItemAtURL:dir error:error];
}

+ (NSDictionary *)readConfigFor:(NSString *)name {
    NSData *data = [NSData dataWithContentsOfURL:[self configURLFor:name]];
    if (!data) return nil;
    id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return [obj isKindOfClass:[NSDictionary class]] ? obj : nil;
}

+ (BOOL)writeConfig:(NSDictionary *)config for:(NSString *)name error:(NSError **)error {
    NSData *data = [NSJSONSerialization dataWithJSONObject:config
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:error];
    if (!data) return NO;
    return [data writeToURL:[self configURLFor:name] options:NSDataWritingAtomic error:error];
}

@end
