#import "../../tools/transport/accessibility_wire_mac.h"

@class VmAccessibilityConnection;
@protocol VmAccessibilityConnectionDelegate <NSObject>
- (void)receiveState:(id)state connection:(VmAccessibilityConnection *)connection;
- (void)connectionEnded:(VmAccessibilityConnection *)connection;
@end

@interface VmAccessibilityConnection : NSObject
@property (weak) id<VmAccessibilityConnectionDelegate> delegate;
@property (readonly) BOOL supportsRefresh;
@property (readonly) BOOL supportsNavigationQueries;
- (instancetype)initWithFileDescriptor:(int)fd;
- (void)begin;
- (void)refresh:(uint64_t)generation;
- (void)stop;
- (NSDictionary *)request:(NSDictionary *)message;
- (NSTimeInterval)lastActivity;
@end
