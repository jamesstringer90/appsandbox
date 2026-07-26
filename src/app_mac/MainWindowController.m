#import "MainWindowController.h"
#import "ui.h"

@interface MainWindowController ()
@property (nonatomic) BOOL borderlessFullscreen;
@property (nonatomic) NSWindowStyleMask windowedStyleMask;
@property (nonatomic) NSRect windowedFrame;
@end

@implementation MainWindowController

- (instancetype)init {
    NSRect frame = NSMakeRect(0, 0, 1280, 800);
    NSUInteger style = NSWindowStyleMaskTitled
                     | NSWindowStyleMaskClosable
                     | NSWindowStyleMaskMiniaturizable
                     | NSWindowStyleMaskResizable;
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.title = @"App Sandbox";
    [window center];

    self = [super initWithWindow:window];
    if (!self) return nil;

    WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
    [config.userContentController addScriptMessageHandler:self name:@"host"];

    self.webView = [[WKWebView alloc] initWithFrame:frame configuration:config];
    self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.webView.navigationDelegate = self;
    window.contentView = self.webView;

    ui_set_webview(self.webView);

    NSString *resourcePath = [[NSBundle mainBundle] pathForResource:@"index"
                                                             ofType:@"html"
                                                        inDirectory:@"web"];
    if (resourcePath) {
        NSURL *url = [NSURL fileURLWithPath:resourcePath];
        NSURL *baseURL = [url URLByDeletingLastPathComponent];
        [self.webView loadFileURL:url allowingReadAccessToURL:baseURL];
    } else {
        NSString *html = @"<html><body style='font-family:sans-serif;padding:40px'>"
                         @"<h2>web/ resources not found in bundle</h2>"
                         @"<p>Add the <code>web/</code> folder to the target's "
                         @"Copy Bundle Resources build phase.</p></body></html>";
        [self.webView loadHTMLString:html baseURL:nil];
    }
    return self;
}

- (void)userContentController:(WKUserContentController *)userContentController
      didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)userContentController;
    if ([message.body isKindOfClass:[NSString class]]) {
        ui_handle_message((NSString *)message.body);
    }
}

- (void)toggleBorderlessFullscreen:(id)sender {
    (void)sender;
    NSWindow *window = self.window;

    if (!self.borderlessFullscreen) {
        self.windowedStyleMask = window.styleMask;
        self.windowedFrame = window.frame;
        window.styleMask = NSWindowStyleMaskBorderless;
        [window setFrame:window.screen.frame display:YES];
        self.borderlessFullscreen = YES;
    } else {
        window.styleMask = self.windowedStyleMask;
        [window setFrame:self.windowedFrame display:YES];
        self.borderlessFullscreen = NO;
    }
}

@end
