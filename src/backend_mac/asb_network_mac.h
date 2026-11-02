#import <Foundation/Foundation.h>
#import <Virtualization/Virtualization.h>
#import <vmnet/vmnet.h>

NS_ASSUME_NONNULL_BEGIN

@interface AsbNatNetwork : NSObject
+ (nullable instancetype)sharedNetworkWithError:(NSError **)error;
+ (void)reset;
@property (nonatomic, readonly) vmnet_network_ref network;
@property (nonatomic, copy, readonly) NSString *subnet;
@end

@interface AsbNatPort : NSObject
- (nullable instancetype)initWithNetwork:(AsbNatNetwork *)network
                                   name:(NSString *)name error:(NSError **)error;
- (nullable instancetype)initWithNetwork:(AsbNatNetwork *)network
                                   name:(NSString *)name datagram:(BOOL)datagram error:(NSError **)error;
@property (atomic, copy, nullable) void (^onFailure)(NSError *error);
- (void)startWithCompletion:(void (^)(NSError *_Nullable error))completion;
- (BOOL)launchTask:(NSTask *)task error:(NSError **)error;
- (void)stop;
@end

NS_ASSUME_NONNULL_END
