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

/* Download the latest supported macOS restore image to ipswURL (unprivileged).
 * vmName keys the launched NSTask so cancelFetchForVm: can terminate it if
 * the VM is deleted mid-download. */
+ (void)fetchLatestIpswToURL:(NSURL *)ipswURL
                        forVm:(NSString *)vmName
                     progress:(nullable IsoPatchProgress)progress
                   completion:(IsoPatchCompletion)completion;

/* Terminate any in-flight fetch-ipsw subprocess registered under vmName.
 * No-op if there is no active fetch. */
+ (void)cancelFetchForVm:(NSString *)vmName;

/* Create disk.img/aux/hwmodel/machine-id in vmDir and run VZMacOSInstaller
 * (unprivileged). */
+ (void)installMacOSWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                    ipswURL:(NSURL *)ipswURL
                       ramMb:(int)ramMb
                       cpus:(int)cpus
                      diskGb:(int)diskGb
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

/* Free cached AuthorizationRef. Call from asb_mac_cleanup. */
+ (void)releaseAuthorization;

@end

NS_ASSUME_NONNULL_END
