#import <Cocoa/Cocoa.h>
#import <Virtualization/Virtualization.h>

@class AsbIvshmemTransport;

@interface VmAccessibilityMac : NSObject
@property(nonatomic, readonly) BOOL hasAccessibilityContent;
- (instancetype)initWithView:(NSView *)view vmName:(NSString *)name;
- (void)configureWithSocketDevice:(VZVirtioSocketDevice *)device virtualMachine:(VZVirtualMachine *)machine;
- (void)configureWithTransport:(AsbIvshmemTransport *)transport;
- (void)startAccessibility;
- (void)stopAccessibility;
- (void)observeWindow;
- (void)geometryDidChange;
- (void)updateDisplayPixels:(NSSize)pixels;
- (NSString *)accessibilityLabel;
- (NSArray *)accessibilityChildren;
- (id)accessibilityFocusedUIElement;
- (id)accessibilityHitTest:(NSPoint)point;
@end
