#import <Cocoa/Cocoa.h>

@interface VmAccessibilitySession : NSObject
@property(nonatomic, weak, readonly) NSView *view;
@property(nonatomic, readonly) BOOL connected;
@property(nonatomic, readonly) BOOL hasAccessibilityContent;
- (instancetype)initWithView:(NSView *)view name:(NSString *)name;
- (void)start;
- (void)stop;
- (BOOL)attachFileDescriptor:(int)fd;
- (void)noteInput;
- (void)observeWindow;
- (void)geometryDidChange;
- (void)setDisplayPixels:(NSSize)pixels available:(BOOL)available;
- (NSString *)accessibilityLabel;
- (NSArray *)accessibilityChildren;
- (id)accessibilityFocusedUIElement;
- (id)accessibilityHitTest:(NSPoint)point;
@end
