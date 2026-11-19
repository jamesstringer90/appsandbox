#import <Cocoa/Cocoa.h>
#include <stdint.h>

FOUNDATION_EXPORT NSString *const AsbAgentOverlayLayerName;
@class AsbAgentOverlay;

@interface AsbViewerWindow:NSWindow
@property(nonatomic,readonly) BOOL viewerFullscreen;
@property(nonatomic,weak) AsbAgentOverlay *viewerOverlay;
- (BOOL)setFullscreenEnabled:(BOOL)enabled;
- (void)toggleViewerFullscreen:(id)sender;
@end

@interface AsbAgentOverlay:NSObject
@property(nonatomic,readonly) BOOL blocksInput;
@property(nonatomic,readonly) BOOL ready;
@property(nonatomic,copy) void (^prepareForAgentInput)(void);
@property(nonatomic,copy) NSDictionary *(^viewerOptions)(NSUInteger option);
- (instancetype)initWithView:(NSView *)view vmId:(uint64_t)identity;
- (BOOL)handleViewerMouseEvent:(NSEvent *)event;
- (void)cancelInput;
- (void)refresh;
- (void)invalidate;
@end
