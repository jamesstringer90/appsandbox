/*
 * vz_network -- Network device configuration helpers.
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

NS_ASSUME_NONNULL_BEGIN

@interface VzNetwork : NSObject

+ (nullable VZVirtioNetworkDeviceConfiguration *)natConfigurationWithMACAddress:(VZMACAddress *)macAddress
                                                                         error:(NSError **)error;

/* Bridged network device for the given interface identifier (e.g. "en0")
 * or the first interface if interfaceName is nil. Returns nil if no interfaces
 * are available. Requires the `com.apple.vm.networking` entitlement. */
+ (nullable VZVirtioNetworkDeviceConfiguration *)bridgedConfigurationForInterface:(nullable NSString *)interfaceName
                                                                     MACAddress:(VZMACAddress *)macAddress;

@end

NS_ASSUME_NONNULL_END
