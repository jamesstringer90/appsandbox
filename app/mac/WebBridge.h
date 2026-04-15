#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

/* Register the WKWebView that future host->JS messages should target. */
void WebBridgeSetWebView(WKWebView *webView);

/* Post a JSON string from the native host to the JS frontend.
   Calls window.onHostMessage(json) on the main thread. */
void WebBridgePostJson(NSString *json);

/* Dispatch a JSON message received from the JS frontend to the core
   message handler. Currently logs and discards. */
void WebBridgeHandleMessage(NSString *json);
