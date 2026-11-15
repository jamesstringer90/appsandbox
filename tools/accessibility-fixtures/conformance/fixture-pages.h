#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
@class ASBFixtureView;

@interface ASBFixturePage : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSOutlineViewDataSource, NSOutlineViewDelegate, NSTextViewDelegate, WKNavigationDelegate>
@property(readonly) NSView *view;
@property(readonly) NSString *name;
- (instancetype)initWithName:(NSString *)name fixture:(ASBFixtureView *)fixture;
- (void)reset;
- (void)configureStress:(NSDictionary *)configuration;
- (NSDictionary *)state;
@end
