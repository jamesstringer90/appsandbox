/*
 * vz_network -- Network device configuration helpers.
 */

#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>

@interface VzNetwork : NSObject

/* NAT-attached virtio network device. Works out of the box, no entitlements. */
+ (VZVirtioNetworkDeviceConfiguration *)natConfiguration;

/* Bridged network device for the given interface identifier (e.g. "en0")
 * or the first interface if interfaceName is nil. Returns nil if no interfaces
 * are available. Requires the `com.apple.vm.networking` entitlement. */
+ (nullable VZVirtioNetworkDeviceConfiguration *)bridgedConfigurationForInterface:(nullable NSString *)interfaceName;

@end
