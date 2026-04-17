#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

void ui_set_webview(WKWebView *webView);
void ui_post_json(NSString *json);
void ui_handle_message(NSString *json);
