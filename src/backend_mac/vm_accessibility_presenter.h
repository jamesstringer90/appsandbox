#import "vm_accessibility_appkit.h"

@class VmAccessibilityPresenter;
@protocol VmAccessibilityPresenterDelegate <NSObject>
- (void)receiveRemoteMessage:(NSDictionary *)message presenter:(VmAccessibilityPresenter *)presenter;
- (void)presenterEnded:(VmAccessibilityPresenter *)presenter;
@end

@interface VmAccessibilityPresenter : NSObject
@property (weak) id<VmAccessibilityPresenterDelegate> delegate;
@property (readonly) NSAccessibilityRemoteUIElement *root;
- (void)start;
- (void)send:(NSDictionary *)message;
- (void)stop;
@end
