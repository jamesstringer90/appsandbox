#import "host_info.h"
#import "vm_dir.h"

#import <Metal/Metal.h>
#include <sys/sysctl.h>
#include <limits.h>

@implementation HostInfo

+ (int)hostCores {
    int ncpu = 0;
    size_t sz = sizeof(ncpu);
    if (sysctlbyname("hw.physicalcpu", &ncpu, &sz, NULL, 0) != 0 || ncpu <= 0) {
        ncpu = (int)[[NSProcessInfo processInfo] activeProcessorCount];
    }
    return ncpu;
}

+ (int)hostRamMb {
    uint64_t bytes = [[NSProcessInfo processInfo] physicalMemory];
    return (int)(bytes / (1024ULL * 1024ULL));
}

+ (int)freeGb {
    return MAX(0, [self freeGbForDirectory:@""]);
}

+ (int)freeGbForDirectory:(NSString *)directory {
    NSString *path = directory.length ? directory : [VmDir vmsRootDirectory].path;
    if (![path hasPrefix:@"/"] ||
        [path rangeOfCharacterFromSet:[NSCharacterSet controlCharacterSet]].location != NSNotFound)
        return -1;

    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDirectory = NO;
    if (![fm fileExistsAtPath:path isDirectory:&isDirectory] || !isDirectory)
        return -1;

    NSError *err = nil;
    NSDictionary *attrs = [fm attributesOfFileSystemForPath:path error:&err];
    if (!attrs || err) return -1;

    NSNumber *freeBytes = attrs[NSFileSystemFreeSize];
    if (!freeBytes) return -1;
    unsigned long long freeGb = [freeBytes unsignedLongLongValue] / (1024ULL * 1024ULL * 1024ULL);
    return freeGb > INT_MAX ? INT_MAX : (int)freeGb;
}

+ (NSString *)hostGpuName {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSString *name = [device.name copy];
    return name ?: @"";
}

@end
