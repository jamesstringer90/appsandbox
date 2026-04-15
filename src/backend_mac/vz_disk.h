/*
 * vz_disk -- Raw disk image creation and copy helpers.
 */

#import <Foundation/Foundation.h>

@interface VzDisk : NSObject

/* Create a sparse raw disk image of the given size in bytes.
 * The file is created with ftruncate() so the filesystem can store it
 * sparsely. Returns YES on success. */
+ (BOOL)createDiskImageAtURL:(NSURL *)url
                   sizeBytes:(uint64_t)sizeBytes
                       error:(NSError **)error;

/* Cold-copy a disk image (used for snapshots). The VM must be stopped. */
+ (BOOL)copyDiskImageFrom:(NSURL *)src to:(NSURL *)dst error:(NSError **)error;

@end
