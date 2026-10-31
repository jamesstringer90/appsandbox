#import "vz_accessibility_view.h"
#import "vm_accessibility_mac.h"
#import "keyboard_modifiers_mac.h"

@implementation VzAccessibilityView {
    VmAccessibilityMac *_accessibilityBridge;
    NSEventModifierFlags _keyboardModifierFlags;
}
- (void)flagsChanged:(NSEvent *)event {
    for (NSEvent *transition in AsbKeyboardModifierEvents(event, &_keyboardModifierFlags))
        [super flagsChanged:transition];
}
- (void)resetKeyboardModifiers { _keyboardModifierFlags = 0; }
- (BOOL)resignFirstResponder {
    BOOL resigned = [super resignFirstResponder];
    if (resigned) [self resetKeyboardModifiers];
    return resigned;
}
- (void)configureWithSocketDevice:(VZVirtioSocketDevice *)device vmName:(NSString *)name {
    [_accessibilityBridge stopAccessibility];
    _accessibilityBridge = [[VmAccessibilityMac alloc] initWithView:self vmName:name];
    [_accessibilityBridge configureWithSocketDevice:device virtualMachine:self.virtualMachine];
}
- (void)startAccessibility { [_accessibilityBridge startAccessibility]; }
- (void)stopAccessibility { [_accessibilityBridge stopAccessibility]; }
- (void)dealloc { [_accessibilityBridge stopAccessibility]; }
- (BOOL)isAccessibilityElement { return _accessibilityBridge.hasAccessibilityContent ? YES : [super isAccessibilityElement]; }
- (NSString *)accessibilityRole { return _accessibilityBridge.hasAccessibilityContent ? NSAccessibilityGroupRole : [super accessibilityRole]; }
- (NSString *)accessibilityLabel { return _accessibilityBridge.hasAccessibilityContent ? _accessibilityBridge.accessibilityLabel : [super accessibilityLabel]; }
- (NSArray *)accessibilityChildren { return _accessibilityBridge.hasAccessibilityContent ? _accessibilityBridge.accessibilityChildren : [super accessibilityChildren]; }
- (NSArray *)accessibilityVisibleChildren { return _accessibilityBridge.hasAccessibilityContent ? _accessibilityBridge.accessibilityChildren : [super accessibilityVisibleChildren]; }
- (id)accessibilityFocusedUIElement { return [_accessibilityBridge accessibilityFocusedUIElement] ?: [super accessibilityFocusedUIElement]; }
- (id)accessibilityHitTest:(NSPoint)point { return [_accessibilityBridge accessibilityHitTest:point] ?: [super accessibilityHitTest:point]; }
- (void)viewDidMoveToWindow { [super viewDidMoveToWindow]; [_accessibilityBridge observeWindow]; }
- (void)setFrameSize:(NSSize)size { [super setFrameSize:size]; [_accessibilityBridge geometryDidChange]; }
- (void)setBoundsSize:(NSSize)size { [super setBoundsSize:size]; [_accessibilityBridge geometryDidChange]; }
@end
