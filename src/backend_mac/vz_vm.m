#import "vz_vm.h"
#import "vm_dir.h"
#import "vz_network.h"

@interface VzVm ()
@property (nonatomic, strong, readwrite) VZVirtualMachine *machine;
@property (nonatomic, strong, readwrite) NSString *name;
@end

static VZVirtioSoundDeviceConfiguration *BuildAudio(BOOL withInput) {
    VZVirtioSoundDeviceConfiguration *audio = [[VZVirtioSoundDeviceConfiguration alloc] init];
    NSMutableArray *streams = [NSMutableArray array];
    if (withInput) {
        VZVirtioSoundDeviceInputStreamConfiguration *input =
            [[VZVirtioSoundDeviceInputStreamConfiguration alloc] init];
        input.source = [[VZHostAudioInputStreamSource alloc] init];
        [streams addObject:input];
    }
    VZVirtioSoundDeviceOutputStreamConfiguration *output =
        [[VZVirtioSoundDeviceOutputStreamConfiguration alloc] init];
    output.sink = [[VZHostAudioOutputStreamSink alloc] init];
    [streams addObject:output];
    audio.streams = streams;
    return audio;
}

static VZMacGraphicsDeviceConfiguration *BuildGraphics(void) {
    VZMacGraphicsDeviceConfiguration *gfx = [[VZMacGraphicsDeviceConfiguration alloc] init];
    gfx.displays = @[[[VZMacGraphicsDisplayConfiguration alloc]
                          initWithWidthInPixels:2560
                                 heightInPixels:1600
                                  pixelsPerInch:144]];
    return gfx;
}

@implementation VzVm

+ (VZVirtualMachineConfiguration *)buildInstallConfigurationForName:(NSString *)name
                                                      hardwareModel:(VZMacHardwareModel *)hardwareModel
                                                   auxiliaryStorage:(VZMacAuxiliaryStorage *)aux
                                                  machineIdentifier:(VZMacMachineIdentifier *)machineId
                                                              ramMb:(int)ramMb
                                                           cpuCount:(int)cpuCount
                                                              error:(NSError **)error {
    VZVirtualMachineConfiguration *config = [[VZVirtualMachineConfiguration alloc] init];

    config.CPUCount = (NSUInteger)MAX(cpuCount, 1);
    config.memorySize = (uint64_t)ramMb * 1024ULL * 1024ULL;

    VZMacPlatformConfiguration *platform = [[VZMacPlatformConfiguration alloc] init];
    platform.hardwareModel = hardwareModel;
    platform.auxiliaryStorage = aux;
    platform.machineIdentifier = machineId;
    config.platform = platform;

    config.bootLoader = [[VZMacOSBootLoader alloc] init];

    NSURL *diskURL = [VmDir diskImageURLFor:name];
    VZDiskImageStorageDeviceAttachment *att =
        [[VZDiskImageStorageDeviceAttachment alloc] initWithURL:diskURL
                                                       readOnly:NO
                                                          error:error];
    if (!att) return nil;
    config.storageDevices = @[[[VZVirtioBlockDeviceConfiguration alloc] initWithAttachment:att]];

    config.graphicsDevices = @[BuildGraphics()];
    config.networkDevices = @[[VzNetwork natConfiguration]];
    config.pointingDevices = @[[[VZMacTrackpadConfiguration alloc] init],
                                [[VZUSBScreenCoordinatePointingDeviceConfiguration alloc] init]];
    config.keyboards = @[[[VZUSBKeyboardConfiguration alloc] init]];
    config.audioDevices = @[BuildAudio(YES)];

    if (![config validateWithError:error]) return nil;
    return config;
}

+ (VzVm *)loadVmNamed:(NSString *)name error:(NSError **)error {
    NSData *hwData = [NSData dataWithContentsOfURL:[VmDir hardwareModelURLFor:name]];
    NSData *midData = [NSData dataWithContentsOfURL:[VmDir machineIdentifierURLFor:name]];
    if (!hwData || !midData) {
        if (error) *error = [NSError errorWithDomain:@"VzVm" code:1
                                              userInfo:@{NSLocalizedDescriptionKey:
                                                            @"Missing hardware model or machine identifier"}];
        return nil;
    }

    VZMacHardwareModel *hw = [[VZMacHardwareModel alloc] initWithDataRepresentation:hwData];
    VZMacMachineIdentifier *mid = [[VZMacMachineIdentifier alloc] initWithDataRepresentation:midData];
    if (!hw || !hw.supported || !mid) {
        if (error) *error = [NSError errorWithDomain:@"VzVm" code:2
                                              userInfo:@{NSLocalizedDescriptionKey:
                                                            @"Unsupported hardware model on this host"}];
        return nil;
    }

    VZMacAuxiliaryStorage *aux = [[VZMacAuxiliaryStorage alloc]
                                    initWithURL:[VmDir auxiliaryStorageURLFor:name]];

    VZMacPlatformConfiguration *platform = [[VZMacPlatformConfiguration alloc] init];
    platform.hardwareModel = hw;
    platform.machineIdentifier = mid;
    platform.auxiliaryStorage = aux;

    VZDiskImageStorageDeviceAttachment *att =
        [[VZDiskImageStorageDeviceAttachment alloc] initWithURL:[VmDir diskImageURLFor:name]
                                                       readOnly:NO
                                                          error:error];
    if (!att) return nil;

    NSDictionary *cfgDict = [VmDir readConfigFor:name];
    int ramMb = [cfgDict[@"ramMb"] intValue];
    int cpuCores = [cfgDict[@"cpuCores"] intValue];
    if (ramMb <= 0) ramMb = 8192;
    if (cpuCores <= 0) cpuCores = 4;

    VZVirtualMachineConfiguration *config = [[VZVirtualMachineConfiguration alloc] init];
    config.CPUCount = (NSUInteger)cpuCores;
    config.memorySize = (uint64_t)ramMb * 1024ULL * 1024ULL;
    config.platform = platform;
    config.bootLoader = [[VZMacOSBootLoader alloc] init];
    config.storageDevices = @[[[VZVirtioBlockDeviceConfiguration alloc] initWithAttachment:att]];
    config.graphicsDevices = @[BuildGraphics()];
    config.networkDevices = @[[VzNetwork natConfiguration]];
    config.pointingDevices = @[[[VZMacTrackpadConfiguration alloc] init],
                                [[VZUSBScreenCoordinatePointingDeviceConfiguration alloc] init]];
    config.keyboards = @[[[VZUSBKeyboardConfiguration alloc] init]];
    config.audioDevices = @[BuildAudio(YES)];

    if (![config validateWithError:error]) return nil;

    VzVm *vm = [[VzVm alloc] init];
    vm.name = name;
    vm.machine = [[VZVirtualMachine alloc] initWithConfiguration:config];
    return vm;
}

- (void)startWithCompletion:(void (^)(NSError * _Nullable))completion {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.machine startWithCompletionHandler:^(NSError * _Nullable err) {
            if (completion) completion(err);
        }];
    });
}

- (void)stopWithCompletion:(void (^)(NSError * _Nullable))completion {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.machine stopWithCompletionHandler:^(NSError * _Nullable err) {
            if (completion) completion(err);
        }];
    });
}

- (void)requestStopWithCompletion:(void (^)(NSError * _Nullable))completion {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSError *err = nil;
        BOOL ok = [self.machine requestStopWithError:&err];
        if (completion) completion(ok ? nil : err);
    });
}

@end
