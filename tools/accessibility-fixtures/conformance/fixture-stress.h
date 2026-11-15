#import <Cocoa/Cocoa.h>

@interface ASBStressFixtureView : NSView
- (void)configure:(NSDictionary *)configuration;
- (void)stop:(id)sender;
- (NSDictionary *)state;
@end
