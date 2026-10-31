/*
 * iso_patch_mac -- Host-side wrapper around the iso-patch-mac CLI.
 *
 * Mirrors backend_win's spawn-and-parse integration with tools/iso-patch
 * on Windows. The CLI owns the whole disk-build pipeline; this wrapper
 * drives it via NSTask for the unprivileged steps (fetch-ipsw, install)
 * and via AuthorizationExecuteWithPrivileges for the privileged step
 * (stage). The user is prompted once per host session for the stage
 * step; subsequent stage calls reuse the cached AuthorizationRef.
 */

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

/* Acquire the AuthorizationRef needed by stageAgentIntoDiskAtURL: ahead of
 * time. Call this at the start of VM creation so the user is prompted once,
 * immediately, rather than 20 minutes into the install. Returns NO if the
 * user cancels. The ref is cached for the process lifetime. */
+ (BOOL)preauthorize:(NSError * _Nullable * _Nullable)error;

/* Download the latest supported macOS restore image to ipswURL (unprivileged). */
+ (void)fetchLatestIpswToURL:(NSURL *)ipswURL
                        forVm:(NSString *)vmName
                     progress:(nullable IsoPatchProgress)progress
                   completion:(IsoPatchCompletion)completion;

/* Terminate any in-flight fetch for vmName. */
+ (void)cancelFetchForVm:(NSString *)vmName;

/* Create disk.img/aux/hwmodel/machine-id in vmDir and run VZMacOSInstaller
 * (unprivileged). */
+ (void)installMacOSWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                    ipswURL:(NSURL *)ipswURL
                       ramMb:(int)ramMb
                       cpus:(int)cpus
                      diskGb:(int)diskGb
                  macAddress:(NSString *)macAddress
                   progress:(nullable IsoPatchProgress)progress
                 completion:(IsoPatchCompletion)completion;

/* Stage the agent bundle + inject a pre-created admin user + skip Setup
 * Assistant + enable auto-login, all in one privileged (AEWP) mount cycle.
 * Pass sshEnabled=YES to also flip sshd's launchd override so the SSH
 * proxy can reach 127.0.0.1:22 on first boot. computerName is the free-form
 * display name shown inside the guest (Sharing prefs, hostname, Bonjour). */
+ (void)stageAgentIntoDiskAtURL:(NSURL *)diskURL
               agentResourceDir:(NSString *)agentResDir
                       adminUser:(NSString *)adminUser
                       adminPass:(NSString *)adminPass
                    computerName:(NSString *)computerName
                     sshEnabled:(BOOL)sshEnabled
                       progress:(nullable IsoPatchProgress)progress
                     completion:(IsoPatchCompletion)completion;

/* Build a from-scratch Windows VM disk (unprivileged): mount the ISO, apply
 * install.wim with our own NTFS writer, and stage the answer file + agent +
 * drivers. Mirrors the Windows VHDX-first create path. The guest payload comes
 * ONLY from signedPayloadZip (required): a cached signed release zip (EV-signed
 * agent EXEs + attestation-signed VDD/VAD/AppSandboxSHM + devcon) that build-windows
 * extracts -- there is no bundled fallback. netkvmZip, when non-nil, is the cached
 * vendored NetKVM zip (virtio-net); without it the VM builds with no guest NIC. */
+ (void)buildWindowsDiskWithISO:(NSURL *)isoURL
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

/* Free cached AuthorizationRef. Call from asb_mac_cleanup. */
+ (void)releaseAuthorization;

@end

NS_ASSUME_NONNULL_END
