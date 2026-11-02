#import "vz_network.h"
#import "asb_network_mac.h"

@implementation VzNetwork

+ (VZVirtioNetworkDeviceConfiguration *)natConfigurationWithMACAddress:(VZMACAddress *)macAddress
                                                               error:(NSError **)error {
    AsbNatNetwork *network = [AsbNatNetwork sharedNetworkWithError:error];
    if (!network) return nil;
    VZVirtioNetworkDeviceConfiguration *net = [[VZVirtioNetworkDeviceConfiguration alloc] init];
    net.MACAddress = macAddress;
    net.attachment = [[VZVmnetNetworkDeviceAttachment alloc] initWithNetwork:network.network];
    return net;
}

+ (VZVirtioNetworkDeviceConfiguration *)bridgedConfigurationForInterface:(NSString *)interfaceName
                                                            MACAddress:(VZMACAddress *)macAddress {
    NSArray<VZBridgedNetworkInterface *> *interfaces = [VZBridgedNetworkInterface networkInterfaces];
    VZBridgedNetworkInterface *chosen = nil;
    if (interfaceName.length > 0) {
        for (VZBridgedNetworkInterface *iface in interfaces) {
            if ([iface.identifier isEqualToString:interfaceName]) {
                chosen = iface;
                break;
            }
        }
    } else {
        chosen = interfaces.firstObject;
    }
    if (!chosen) return nil;

    VZVirtioNetworkDeviceConfiguration *net = [[VZVirtioNetworkDeviceConfiguration alloc] init];
    net.MACAddress = macAddress;
    net.attachment = [[VZBridgedNetworkDeviceAttachment alloc] initWithInterface:chosen];
    return net;
}

@end
