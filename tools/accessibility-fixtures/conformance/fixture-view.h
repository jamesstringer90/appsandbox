#import <Cocoa/Cocoa.h>

@interface ASBFixtureView : NSView <NSTextFieldDelegate, NSTextViewDelegate>
@property NSTextField *caseField, *stateField, *textField;
@property NSButton *checkbox;
@property NSSlider *slider;
@property NSTextView *editor;
@property NSInteger presses, revision;
@property BOOL resetting;
- (void)publishState;
- (NSString *)stateJSON;
- (void)reset:(id)sender;
@end
