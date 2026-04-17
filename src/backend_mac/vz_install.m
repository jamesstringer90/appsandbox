#import "vz_install.h"
#import "vz_vm.h"
#import "vz_disk.h"
#import "vm_dir.h"

#pragma mark - Download delegate

@interface VzInstallDownloadDelegate : NSObject <NSURLSessionDownloadDelegate>
@property (nonatomic, copy) VzInstallProgressBlock progress;
@property (nonatomic, copy) VzInstallCompletionBlock completion;
@property (nonatomic, strong) NSURL *destinationURL;
@property (nonatomic, assign) BOOL finished;
@end

@implementation VzInstallDownloadDelegate

- (void)finishWithError:(NSError *)error {
    if (self.finished) return;
    self.finished = YES;
    VzInstallCompletionBlock done = self.completion;
    self.completion = nil;
    self.progress = nil;
    if (done) {
        dispatch_async(dispatch_get_main_queue(), ^{ done(error); });
    }
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    (void)session; (void)downloadTask; (void)bytesWritten;
    if (totalBytesExpectedToWrite <= 0) return;
    VzInstallProgressBlock block = self.progress;
    if (!block) return;
    double frac = (double)totalBytesWritten / (double)totalBytesExpectedToWrite;
    NSString *stage = [NSString stringWithFormat:@"Downloading restore image (%.0f%%)", frac * 100.0];
    dispatch_async(dispatch_get_main_queue(), ^{ block(frac, stage); });
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location {
    (void)session; (void)downloadTask;
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm removeItemAtURL:self.destinationURL error:nil];
    NSError *moveErr = nil;
    if (![fm moveItemAtURL:location toURL:self.destinationURL error:&moveErr]) {
        [self finishWithError:moveErr];
    }
    /* Otherwise wait for didCompleteWithError to fire the completion — it always
     * runs after didFinishDownloadingToURL on success, so single-path finish. */
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
    (void)session; (void)task;
    [self finishWithError:error];
}

@end

#pragma mark - Install progress observer

@interface VzInstallProgressObserver : NSObject
@property (nonatomic, strong) NSProgress *observed;
@property (nonatomic, copy) VzInstallProgressBlock progress;
@end

@implementation VzInstallProgressObserver

- (void)startObserving {
    [self.observed addObserver:self
                    forKeyPath:@"fractionCompleted"
                       options:NSKeyValueObservingOptionNew
                       context:NULL];
}

- (void)stopObserving {
    @try { [self.observed removeObserver:self forKeyPath:@"fractionCompleted"]; }
    @catch (__unused NSException *ex) {}
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey,id> *)change
                       context:(void *)context {
    (void)object; (void)change; (void)context;
    if (![keyPath isEqualToString:@"fractionCompleted"]) return;
    double frac = self.observed.fractionCompleted;
    NSString *stage = [NSString stringWithFormat:@"Installing macOS (%.0f%%)", frac * 100.0];
    VzInstallProgressBlock block = self.progress;
    if (block) {
        dispatch_async(dispatch_get_main_queue(), ^{ block(frac, stage); });
    }
}

@end

#pragma mark - VzInstall

@implementation VzInstall

+ (void)fetchLatestRestoreImageURLWithCompletion:(VzRestoreImageURLBlock)completion {
    [VZMacOSRestoreImage fetchLatestSupportedWithCompletionHandler:^(VZMacOSRestoreImage * _Nullable image,
                                                                     NSError * _Nullable error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error || !image) {
                completion(nil, error);
            } else {
                completion(image.URL, nil);
            }
        });
    }];
}

+ (void)downloadRestoreImageFromURL:(NSURL *)remoteURL
                              toURL:(NSURL *)localURL
                           progress:(VzInstallProgressBlock)progress
                         completion:(VzInstallCompletionBlock)completion {
    VzInstallDownloadDelegate *delegate = [[VzInstallDownloadDelegate alloc] init];
    delegate.progress = progress;
    delegate.completion = completion;
    delegate.destinationURL = localURL;

    NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration defaultSessionConfiguration];
    cfg.timeoutIntervalForRequest = 0;
    cfg.timeoutIntervalForResource = 0;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg
                                                           delegate:delegate
                                                      delegateQueue:nil];
    NSURLSessionDownloadTask *task = [session downloadTaskWithURL:remoteURL];
    [task resume];
}


+ (void)installMacOSWithName:(NSString *)name
             restoreImageURL:(NSURL *)restoreImageURL
                       ramMb:(int)ramMb
                       hddGb:(int)hddGb
                    cpuCores:(int)cpuCores
                    progress:(VzInstallProgressBlock)progress
                  completion:(VzInstallCompletionBlock)completion {
    NSError *err = nil;
    if (![VmDir ensureDirectoryFor:name error:&err]) {
        completion(err);
        return;
    }

    if (progress) progress(0.0, @"Loading restore image");

    [VZMacOSRestoreImage loadFileURL:restoreImageURL
                    completionHandler:^(VZMacOSRestoreImage * _Nullable image, NSError * _Nullable loadErr) {
        if (loadErr || !image) {
            dispatch_async(dispatch_get_main_queue(), ^{ completion(loadErr); });
            return;
        }

        VZMacOSConfigurationRequirements *reqs = image.mostFeaturefulSupportedConfiguration;
        if (!reqs) {
            NSError *e = [NSError errorWithDomain:@"VzInstall" code:10
                                          userInfo:@{NSLocalizedDescriptionKey:
                                                        @"Restore image has no supported configuration on this host"}];
            dispatch_async(dispatch_get_main_queue(), ^{ completion(e); });
            return;
        }

        int cpus = MAX(cpuCores, (int)reqs.minimumSupportedCPUCount);
        uint64_t ramBytes = MAX((uint64_t)ramMb * 1024ULL * 1024ULL, reqs.minimumSupportedMemorySize);
        int ramMbFinal = (int)(ramBytes / (1024ULL * 1024ULL));

        NSURL *diskURL = [VmDir diskImageURLFor:name];
        uint64_t diskBytes = (uint64_t)hddGb * 1024ULL * 1024ULL * 1024ULL;
        NSError *diskErr = nil;
        if (![VzDisk createDiskImageAtURL:diskURL sizeBytes:diskBytes error:&diskErr]) {
            dispatch_async(dispatch_get_main_queue(), ^{ completion(diskErr); });
            return;
        }

        VZMacHardwareModel *hw = reqs.hardwareModel;
        NSData *hwData = hw.dataRepresentation;
        if (![hwData writeToURL:[VmDir hardwareModelURLFor:name] atomically:YES]) {
            NSError *e = [NSError errorWithDomain:@"VzInstall" code:11
                                          userInfo:@{NSLocalizedDescriptionKey:
                                                        @"Failed to write hardware model"}];
            dispatch_async(dispatch_get_main_queue(), ^{ completion(e); });
            return;
        }

        VZMacMachineIdentifier *mid = [[VZMacMachineIdentifier alloc] init];
        [mid.dataRepresentation writeToURL:[VmDir machineIdentifierURLFor:name] atomically:YES];

        NSError *auxErr = nil;
        VZMacAuxiliaryStorage *aux =
            [[VZMacAuxiliaryStorage alloc] initCreatingStorageAtURL:[VmDir auxiliaryStorageURLFor:name]
                                                      hardwareModel:hw
                                                            options:VZMacAuxiliaryStorageInitializationOptionAllowOverwrite
                                                              error:&auxErr];
        if (!aux) {
            dispatch_async(dispatch_get_main_queue(), ^{ completion(auxErr); });
            return;
        }

        NSError *buildErr = nil;
        VZVirtualMachineConfiguration *config =
            [VzVm buildInstallConfigurationForName:name
                                      hardwareModel:hw
                                   auxiliaryStorage:aux
                                  machineIdentifier:mid
                                              ramMb:ramMbFinal
                                           cpuCount:cpus
                                              error:&buildErr];
        if (!config) {
            dispatch_async(dispatch_get_main_queue(), ^{ completion(buildErr); });
            return;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            VZVirtualMachine *machine = [[VZVirtualMachine alloc] initWithConfiguration:config];
            VZMacOSInstaller *installer =
                [[VZMacOSInstaller alloc] initWithVirtualMachine:machine
                                                 restoreImageURL:restoreImageURL];

            /* Retain the observer for the duration of the install by capturing it
             * in the completion block below. */
            VzInstallProgressObserver *obs = [[VzInstallProgressObserver alloc] init];
            obs.observed = installer.progress;
            obs.progress = progress;
            [obs startObserving];

            [installer installWithCompletionHandler:^(NSError * _Nullable installErr) {
                [obs stopObserving];
                (void)obs;
                dispatch_async(dispatch_get_main_queue(), ^{ completion(installErr); });
            }];
        });
    }];
}

@end
