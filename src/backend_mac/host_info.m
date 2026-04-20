#import "host_info.h"
#import "vm_dir.h"

#import <Metal/Metal.h>
#include <sys/sysctl.h>

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
    NSURL *root = [VmDir vmsRootDirectory];
    if (!root) return 0;

    NSError *err = nil;
    NSDictionary *attrs = [[NSFileManager defaultManager]
        attributesOfFileSystemForPath:[root path] error:&err];
    if (!attrs) return 0;

    NSNumber *freeBytes = attrs[NSFileSystemFreeSize];
    if (!freeBytes) return 0;
    return (int)([freeBytes unsignedLongLongValue] / (1024ULL * 1024ULL * 1024ULL));
}

+ (NSString *)hostGpuName {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSString *name = [device.name copy];
    return name ?: @"";
}

@end
