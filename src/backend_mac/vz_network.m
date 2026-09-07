#import "vz_network.h"
#import <arpa/inet.h>
#import <vmnet/vmnet.h>

static vmnet_network_ref g_sharedNetwork;

@implementation VzNetwork

+ (vmnet_network_ref)sharedNetworkWithError:(NSError **)error {
    @synchronized (self) {
        if (g_sharedNetwork) return g_sharedNetwork;

        vmnet_return_t status = VMNET_FAILURE;
        vmnet_network_configuration_ref config =
            vmnet_network_configuration_create(VMNET_SHARED_MODE, &status);
        vmnet_network_ref network = nil;
        if (config && status == VMNET_SUCCESS) {
            struct in_addr subnet, mask;
            inet_pton(AF_INET, "192.168.43.0", &subnet);
            inet_pton(AF_INET, "255.255.255.0", &mask);
            status = vmnet_network_configuration_set_ipv4_subnet(config, &subnet, &mask);
            if (status == VMNET_SUCCESS)
                network = vmnet_network_create(config, &status);
        }
        if (!network || status != VMNET_SUCCESS) {
            if (error) *error = [NSError errorWithDomain:@"VzNetwork" code:status
                userInfo:@{NSLocalizedDescriptionKey:
                    [NSString stringWithFormat:@"Could not create the VM DHCP network (192.168.43.0/24, error %d).", status]}];
            return nil;
        }
        g_sharedNetwork = network;
        return g_sharedNetwork;
    }
}

+ (VZVirtioNetworkDeviceConfiguration *)natConfigurationWithError:(NSError **)error {
    vmnet_network_ref network = [self sharedNetworkWithError:error];
    if (!network) return nil;
    VZVmnetNetworkDeviceAttachment *attachment =
        [[VZVmnetNetworkDeviceAttachment alloc] initWithNetwork:network];
    if (!attachment) {
        if (error) *error = [NSError errorWithDomain:@"VzNetwork" code:VMNET_FAILURE
            userInfo:@{NSLocalizedDescriptionKey:@"Could not attach the VM to its DHCP network."}];
        return nil;
    }
    VZVirtioNetworkDeviceConfiguration *net = [[VZVirtioNetworkDeviceConfiguration alloc] init];
    net.attachment = attachment;
    return net;
}

+ (VZVirtioNetworkDeviceConfiguration *)bridgedConfigurationForInterface:(NSString *)interfaceName {
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
    net.attachment = [[VZBridgedNetworkDeviceAttachment alloc] initWithInterface:chosen];
    return net;
}

@end
