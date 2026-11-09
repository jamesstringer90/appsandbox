#import <Cocoa/Cocoa.h>
#include <stdint.h>

@interface AsbAgentOverlay:NSObject
@property(nonatomic,readonly) BOOL blocksInput;
- (instancetype)initWithView:(NSView *)view vmId:(uint64_t)identity;
- (BOOL)handleEvent:(NSEvent *)event;
- (void)invalidate;
@end
