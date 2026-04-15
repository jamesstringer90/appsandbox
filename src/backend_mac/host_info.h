/*
 * host_info -- Host machine resource snapshot for the macOS backend.
 *
 * Mirrors the fields produced by the Windows ui.c build_host_info_json
 * so the shared web UI can render the same "Host" panel on both
 * platforms.
 */

#import <Foundation/Foundation.h>

@interface HostInfo : NSObject

/* Physical cores reported by the kernel. */
+ (int)hostCores;

/* Physical RAM in megabytes. */
+ (int)hostRamMb;

/* Free bytes on the volume that backs the VMs root directory, as GB. */
+ (int)freeGb;

@end
