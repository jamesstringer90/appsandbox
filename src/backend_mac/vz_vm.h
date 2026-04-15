/*
 * vz_vm -- Wraps VZVirtualMachine with load/start/stop helpers.
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^VzVmStateChangeBlock)(VZVirtualMachineState state);

@interface VzVm : NSObject

@property (nonatomic, strong, readonly) VZVirtualMachine *machine;
@property (nonatomic, strong, readonly) NSString *name;
@property (nonatomic, copy, nullable) VzVmStateChangeBlock onStateChange;

/* Build a VZVirtualMachineConfiguration from on-disk state for the named VM.
 * Returns a ready-to-start wrapper, or nil on error. */
+ (nullable VzVm *)loadVmNamed:(NSString *)name error:(NSError **)error;

/* Build a fresh configuration for install (no disks are loaded from disk.img;
 * this is used by vz_install during VZMacOSInstaller setup). */
+ (nullable VZVirtualMachineConfiguration *)buildInstallConfigurationForName:(NSString *)name
                                                      hardwareModel:(VZMacHardwareModel *)hardwareModel
                                                   auxiliaryStorage:(VZMacAuxiliaryStorage *)aux
                                                  machineIdentifier:(VZMacMachineIdentifier *)machineId
                                                              ramMb:(int)ramMb
                                                           cpuCount:(int)cpuCount
                                                              error:(NSError **)error;

- (void)startWithCompletion:(void (^)(NSError * _Nullable))completion;
- (void)stopWithCompletion:(void (^)(NSError * _Nullable))completion;
- (void)requestStopWithCompletion:(void (^)(NSError * _Nullable))completion;

@end

NS_ASSUME_NONNULL_END
