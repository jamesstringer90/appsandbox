#import <Cocoa/Cocoa.h>

@interface NSAccessibilityRemoteUIElement : NSObject
+ (void)setRemoteUIApp:(BOOL)value;
+ (void)registerRemoteUIProcessIdentifier:(pid_t)pid;
+ (void)unregisterRemoteUIProcessIdentifier:(pid_t)pid;
+ (NSData *)remoteTokenForLocalUIElement:(id)element;
- (id)initWithRemoteToken:(NSData *)data;
@property id windowUIElement;
@property id topLevelUIElement;
@end

@interface NSObject (VmAccessibilityPresenterIdentity)
- (void)accessibilitySetPresenterProcessIdentifier:(pid_t)pid;
@end

static inline BOOL VmAccessibilityIsInputEvent(NSEvent *event) {
    switch (event.type) {
        case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown: case NSEventTypeOtherMouseUp:
        case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
        case NSEventTypeKeyDown: case NSEventTypeKeyUp:
        case NSEventTypeFlagsChanged: case NSEventTypeScrollWheel:
            return YES;
        default: return NO;
    }
}
