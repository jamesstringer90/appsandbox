#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@interface MainWindowController : NSWindowController <WKNavigationDelegate, WKScriptMessageHandler>
@property (nonatomic, strong) WKWebView *webView;
- (void)toggleBorderlessFullscreen:(id)sender;
@end
