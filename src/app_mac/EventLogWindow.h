/*
 * EventLogWindow -- auxiliary developer-facing log window, analogous to
 * the Windows IDD log window. Separate NSWindow with a monospace, dark
 * NSTextView fed by appendLine:. Thread-safe (hops to main). Rolling
 * 200-line buffer; auto-scrolls; Cmd+L toggles visibility.
 *
 * Event sources feeding it: asb_core_mac's post_log(), clipboard,
 * agent, SSH proxy, iso-patch-mac stdout. Anything ui.m surfaces via
 * sendLog() is mirrored here.
 */

#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

@interface EventLogWindow : NSObject

+ (instancetype)shared;

- (void)appendLine:(NSString *)line;
- (void)show;
- (void)hide;
- (void)toggle;

@end

NS_ASSUME_NONNULL_END
