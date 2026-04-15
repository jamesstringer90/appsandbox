#import "vz_disk.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

@implementation VzDisk

+ (BOOL)createDiskImageAtURL:(NSURL *)url
                   sizeBytes:(uint64_t)sizeBytes
                       error:(NSError **)error {
    NSString *path = [url path];
    const char *cpath = [path fileSystemRepresentation];

    int fd = open(cpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                                 code:errno
                                             userInfo:@{NSLocalizedDescriptionKey:
                                                           [NSString stringWithFormat:@"open(%@) failed: %s",
                                                                          path, strerror(errno)]}];
        return NO;
    }
    if (ftruncate(fd, (off_t)sizeBytes) != 0) {
        int e = errno;
        close(fd);
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                                 code:e
                                             userInfo:@{NSLocalizedDescriptionKey:
                                                           [NSString stringWithFormat:@"ftruncate failed: %s",
                                                                          strerror(e)]}];
        return NO;
    }
    close(fd);
    return YES;
}

+ (BOOL)copyDiskImageFrom:(NSURL *)src to:(NSURL *)dst error:(NSError **)error {
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm removeItemAtURL:dst error:nil];
    return [fm copyItemAtURL:src toURL:dst error:error];
}

@end
