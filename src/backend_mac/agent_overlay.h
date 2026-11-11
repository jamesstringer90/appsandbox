#import <Cocoa/Cocoa.h>
#include <stdint.h>

FOUNDATION_EXPORT NSString *const AsbAgentOverlayLayerName;

@interface AsbViewerWindow:NSWindow
@end

@interface AsbAgentOverlay:NSObject
@property(nonatomic,readonly) BOOL blocksInput;
@property(nonatomic,copy) void (^prepareForAgentInput)(void);
- (instancetype)initWithView:(NSView *)view vmId:(uint64_t)identity;
- (BOOL)handleViewerMouseEvent:(NSEvent *)event;
- (void)cancelInput;
- (void)invalidate;
@end
