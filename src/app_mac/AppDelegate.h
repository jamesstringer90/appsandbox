#import <Cocoa/Cocoa.h>

@class MainWindowController;

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) MainWindowController *mainWindowController;
@property (nonatomic, copy) void (^headlessQuitHandler)(void);
@end
