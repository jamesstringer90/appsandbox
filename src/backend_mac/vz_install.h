/*
 * vz_install -- macOS guest install orchestration.
 * Fetches the latest supported restore image, downloads the .ipsw,
 * creates disk/aux/hardware/machine-id state, runs VZMacOSInstaller.
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^VzInstallProgressBlock)(double fraction, NSString *stage);
typedef void (^VzInstallCompletionBlock)(NSError * _Nullable error);
typedef void (^VzRestoreImageURLBlock)(NSURL * _Nullable url, NSError * _Nullable error);

@interface VzInstall : NSObject

/* Query Apple's service for the latest supported macOS restore image URL. */
+ (void)fetchLatestRestoreImageURLWithCompletion:(VzRestoreImageURLBlock)completion;

/* Download a restore image (.ipsw) to a local file, reporting progress. */
+ (void)downloadRestoreImageFromURL:(NSURL *)remoteURL
                              toURL:(NSURL *)localURL
                           progress:(VzInstallProgressBlock)progress
                         completion:(VzInstallCompletionBlock)completion;

/* Install macOS into a new VM directory. Creates the disk image, aux storage,
 * hardware model, and machine identifier, then runs VZMacOSInstaller. */
+ (void)installMacOSWithName:(NSString *)name
             restoreImageURL:(NSURL *)restoreImageURL
                       ramMb:(int)ramMb
                       hddGb:(int)hddGb
                    cpuCores:(int)cpuCores
                    progress:(VzInstallProgressBlock)progress
                  completion:(VzInstallCompletionBlock)completion;

@end

NS_ASSUME_NONNULL_END
