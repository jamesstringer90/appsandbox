#import "EventLogWindow.h"

#define MAX_LOG_LINES 200

@interface EventLogWindow ()
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSTextView *textView;
@property (nonatomic, strong) NSDateFormatter *timestampFormatter;
@property (nonatomic, strong) NSColor *logColor;
@end

@implementation EventLogWindow

+ (instancetype)shared {
    static EventLogWindow *inst;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ inst = [[EventLogWindow alloc] init]; });
    return inst;
}

- (instancetype)init {
    if ((self = [super init])) {
        _timestampFormatter = [[NSDateFormatter alloc] init];
        _timestampFormatter.dateFormat = @"HH:mm:ss";
        _logColor = [NSColor colorWithWhite:0.85 alpha:1.0];
        [self buildOnMain];
    }
    return self;
}

- (void)buildOnMain {
    if ([NSThread isMainThread]) { [self build]; }
    else dispatch_sync(dispatch_get_main_queue(), ^{ [self build]; });
}

- (void)build {
    NSRect frame = NSMakeRect(0, 0, 820, 360);
    NSWindowStyleMask mask = NSWindowStyleMaskTitled
                           | NSWindowStyleMaskClosable
                           | NSWindowStyleMaskMiniaturizable
                           | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:mask
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"AppSandbox Event Log";
    self.window.releasedWhenClosed = NO;
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [self.window setFrameAutosaveName:@"EventLogWindow"];

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:[self.window.contentView bounds]];
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    sv.hasVerticalScroller = YES;
    sv.hasHorizontalScroller = NO;
    sv.borderType = NSNoBorder;
    sv.drawsBackground = YES;
    sv.backgroundColor = [NSColor colorWithWhite:0.08 alpha:1.0];

    NSTextView *tv = [[NSTextView alloc] initWithFrame:sv.bounds];
    tv.minSize = NSMakeSize(0, 0);
    tv.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    tv.verticallyResizable = YES;
    tv.horizontallyResizable = NO;
    tv.autoresizingMask = NSViewWidthSizable;
    tv.editable = NO;
    tv.selectable = YES;
    tv.richText = NO;
    tv.drawsBackground = YES;
    tv.backgroundColor = [NSColor colorWithWhite:0.08 alpha:1.0];
    tv.textColor = self.logColor;
    tv.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    tv.textContainerInset = NSMakeSize(8, 6);
    tv.textContainer.widthTracksTextView = YES;
    tv.textContainer.containerSize = NSMakeSize(sv.contentSize.width, CGFLOAT_MAX);
    sv.documentView = tv;

    self.window.contentView = sv;
    self.textView = tv;

    [self.window center];
}

- (void)appendLine:(NSString *)line {
    if (!line) return;
    /* Strip trailing newlines so we control the separators. */
    NSString *trimmed = [line stringByTrimmingCharactersInSet:
        [NSCharacterSet newlineCharacterSet]];
    if (!trimmed.length) return;

    NSString *ts = [self.timestampFormatter stringFromDate:[NSDate date]];
    NSString *row = [NSString stringWithFormat:@"[%@] %@\n", ts, trimmed];

    dispatch_block_t work = ^{
        NSAttributedString *as = [[NSAttributedString alloc] initWithString:row
            attributes:@{
                NSFontAttributeName:            self.textView.font,
                NSForegroundColorAttributeName: self.logColor,
            }];
        [self.textView.textStorage appendAttributedString:as];

        /* Roll the buffer: keep only the last MAX_LOG_LINES. */
        NSString *all = self.textView.string;
        NSArray *lines = [all componentsSeparatedByString:@"\n"];
        if (lines.count > MAX_LOG_LINES + 1) {
            NSUInteger drop = lines.count - MAX_LOG_LINES - 1;
            NSRange scan = NSMakeRange(0, 0);
            NSUInteger pos = 0;
            for (NSUInteger i = 0; i < drop; i++) {
                NSRange nl = [all rangeOfString:@"\n"
                                        options:0
                                          range:NSMakeRange(pos, all.length - pos)];
                if (nl.location == NSNotFound) break;
                pos = nl.location + 1;
            }
            scan = NSMakeRange(0, pos);
            [self.textView.textStorage deleteCharactersInRange:scan];
        }

        [self.textView scrollRangeToVisible:
            NSMakeRange(self.textView.textStorage.length, 0)];
    };
    if ([NSThread isMainThread]) work(); else dispatch_async(dispatch_get_main_queue(), work);
}

- (void)show {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.window makeKeyAndOrderFront:nil];
    });
}

- (void)hide {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.window orderOut:nil];
    });
}

- (void)toggle {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.window.isVisible) [self.window orderOut:nil];
        else                       [self.window makeKeyAndOrderFront:nil];
    });
}

@end
