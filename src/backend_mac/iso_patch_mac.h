
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* fraction: 0.0..1.0 = real progress; -1.0 = indeterminate (a STATUS phase change);
 * ISO_PATCH_PROGRESS_LOG = the `step` is a log line to surface to the create log (not progress);
 * ISO_PATCH_PROGRESS_LANG = the `step` is the raw detected BCP-47 language tag (e.g. "en-US"),
 *   delivered to buildWindowsDiskWithISO so it can re-generate unattend.xml in-process with the
 *   real language -- mirrors the Windows daemon's LANG: re-gen (asb_core.c vhdx_create_thread).
 *   Consumed internally by build-windows; never reaches the create-flow caller. */
#define ISO_PATCH_PROGRESS_LOG  (-2.0)
#define ISO_PATCH_PROGRESS_LANG (-3.0)
typedef void (^IsoPatchProgress)(double fraction, NSString *step);
typedef void (^IsoPatchCompletion)(NSError * _Nullable error);

@interface IsoPatchMac : NSObject

/* Path to the bundled iso-patch-mac binary; nil if not located. */
+ (nullable NSString *)toolPath;

+ (void)fetchLatestIpswToURL:(NSURL *)ipswURL
                     progress:(nullable IsoPatchProgress)progress
                   completion:(IsoPatchCompletion)completion;

+ (void)stopWorkers;

+ (void)installMacOSWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                    ipswURL:(NSURL *)ipswURL
                       ramMb:(int)ramMb
                       cpus:(int)cpus
                      diskGb:(int)diskGb
                  macAddress:(NSString *)macAddress
                   progress:(nullable IsoPatchProgress)progress
                 completion:(IsoPatchCompletion)completion;

+ (void)stageAgentIntoDiskAtURL:(NSURL *)diskURL
               agentResourceDir:(NSString *)agentResDir
                       adminUser:(NSString *)adminUser
                    computerName:(NSString *)computerName
                          diskGb:(int)diskGb
                     sshEnabled:(BOOL)sshEnabled
                       progress:(nullable IsoPatchProgress)progress
                     completion:(IsoPatchCompletion)completion;

+ (void)buildWindowsDiskWithISO:(nullable NSURL *)isoURL
                        outDisk:(NSURL *)outDiskURL
               signedPayloadZip:(NSString *)signedPayloadZip
                      netkvmZip:(nullable NSString *)netkvmZip
                     sshMsiPath:(nullable NSString *)sshMsiPath
                         vmName:(NSString *)vmName
                      adminUser:(NSString *)adminUser
                      adminPass:(NSString *)adminPass
                           lang:(nullable NSString *)lang
                         diskGb:(int)diskGb
                       testMode:(BOOL)testMode
                   templateMode:(int)templateMode
                       progress:(nullable IsoPatchProgress)progress
                     completion:(IsoPatchCompletion)completion;

/* Ensure the OpenSSH ARM64 MSI is cached locally (downloads it on first use, mirroring the Windows
 * ensure_ssh_msi_cached). Returns the cached path, or nil on failure. Blocking; call off-main. */
+ (nullable NSString *)ensureOpenSSHMsiCached;

/* Ensure the SIGNED Windows guest payload zip (EV-signed agent EXEs + attestation-signed drivers,
 * incl. the ivshmem AppSandboxSHM) is cached locally. Mirrors ensureOpenSSHMsiCached: downloads the
 * release zip on first use (single atomic write; the version is pinned in the filename so a newer
 * release auto-invalidates) and returns the cached zip PATH. build-windows extracts the needed
 * members into its own per-build temp dir. Returns nil on failure (the caller must abort the build;
 * there is no bundled fallback). Blocking; call off-main. */
+ (nullable NSString *)ensureSignedWinPayloadZipCached;

/* Ensure the vendored NetKVM (virtio-net) zip is cached locally. Mirrors ensureSignedWinPayloadZipCached:
 * downloads the small BSD-3 zip from the public repo on first use (single atomic write) and returns the
 * cached PATH. build-windows extracts it into the guest's drivers. Returns nil on failure (the VM then
 * builds without a guest NIC). Blocking; call off-main. */
+ (nullable NSString *)ensureNetkvmZipCached;

@end

NS_ASSUME_NONNULL_END
