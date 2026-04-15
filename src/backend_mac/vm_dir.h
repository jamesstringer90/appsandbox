/*
 * vm_dir -- On-disk layout of a macOS VM.
 *
 * Each VM lives at ~/Library/Application Support/AppSandbox/VMs/<name>/
 * and contains:
 *   disk.img        -- raw disk image (VZDiskImageStorageDeviceAttachment)
 *   aux.img         -- VZMacAuxiliaryStorage (NVRAM + firmware data)
 *   hardware.bin    -- VZMacHardwareModel dataRepresentation
 *   machine-id.bin  -- VZMacMachineIdentifier dataRepresentation
 *   config.json     -- ram/cpu/hdd and other metadata
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface VmDir : NSObject

+ (NSURL *)vmsRootDirectory;
+ (NSURL *)directoryForVm:(NSString *)name;

+ (NSURL *)diskImageURLFor:(NSString *)name;
+ (NSURL *)auxiliaryStorageURLFor:(NSString *)name;
+ (NSURL *)hardwareModelURLFor:(NSString *)name;
+ (NSURL *)machineIdentifierURLFor:(NSString *)name;
+ (NSURL *)configURLFor:(NSString *)name;

+ (BOOL)ensureDirectoryFor:(NSString *)name error:(NSError **)error;
+ (BOOL)vmExists:(NSString *)name;
+ (NSArray<NSString *> *)listAllVmNames;
+ (BOOL)deleteVm:(NSString *)name error:(NSError **)error;

+ (nullable NSDictionary *)readConfigFor:(NSString *)name;
+ (BOOL)writeConfig:(NSDictionary *)config for:(NSString *)name error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
