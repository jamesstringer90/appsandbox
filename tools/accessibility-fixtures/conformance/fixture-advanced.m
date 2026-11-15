#import "fixture-advanced.h"
#import <Accessibility/Accessibility.h>

@interface ASBAdvancedFixtureView : NSView <NSAccessibilityCustomRotorItemSearchDelegate, AXCustomContentProvider, AXMathExpressionProvider>
@property NSArray *items;
@property(nonatomic, copy) NSArray<AXCustomContent *> *accessibilityCustomContent;
@property NSInteger actionCount;
@end
@implementation ASBAdvancedFixtureView
- (BOOL)isAccessibilityElement { return YES; }
- (NSString *)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString *)accessibilityIdentifier { return @"asb-advanced"; }
- (NSString *)accessibilityLabel { return @"Advanced native provider APIs"; }
- (id)accessibilityValue { return @(self.actionCount); }
- (NSArray *)accessibilityChildren { return self.items; }
- (NSArray *)accessibilityCustomActions {
    __weak ASBAdvancedFixtureView *weak = self;
    return @[[[NSAccessibilityCustomAction alloc] initWithName:@"Count custom action" handler:^BOOL {
        weak.actionCount++; NSAccessibilityPostNotification(weak, NSAccessibilityValueChangedNotification); return YES;
    }]];
}
- (NSArray *)accessibilityCustomRotors { return @[[[NSAccessibilityCustomRotor alloc] initWithLabel:@"Fixture headings" itemSearchDelegate:self]]; }
- (NSAccessibilityCustomRotorItemResult *)rotor:(NSAccessibilityCustomRotor *)rotor resultForSearchParameters:(NSAccessibilityCustomRotorSearchParameters *)parameters {
    (void)rotor; NSInteger current = parameters.currentItem ? [self.items indexOfObject:parameters.currentItem.targetElement] : NSNotFound;
    BOOL forward = parameters.searchDirection == NSAccessibilityCustomRotorSearchDirectionNext;
    NSInteger index = current == NSNotFound ? (forward ? 0 : (NSInteger)self.items.count - 1) : current + (forward ? 1 : -1);
    return index < 0 || index >= (NSInteger)self.items.count ? nil : [[NSAccessibilityCustomRotorItemResult alloc] initWithTargetElement:self.items[index]];
}
- (AXMathExpression *)accessibilityMathExpression API_AVAILABLE(macos(15.2)) {
    if (@available(macOS 15.2, *)) return [[AXMathExpressionFraction alloc] initWithNumeratorExpression:[[AXMathExpressionIdentifier alloc] initWithContent:@"x"] denimonatorExpression:[[AXMathExpressionNumber alloc] initWithContent:@"2"]];
    return nil;
}
@end

@interface ASBCatalogElement : NSAccessibilityElement
@property(weak) NSView *ownerView;
@property NSUInteger row;
@end
@implementation ASBCatalogElement
- (NSRect)accessibilityFrame { return NSAccessibilityFrameInView(self.ownerView, NSMakeRect(20, self.row * 30, 440, 24)); }
@end
@interface ASBRoleCatalogView : NSView
@property NSArray *items;
@end
@implementation ASBRoleCatalogView
- (BOOL)isAccessibilityElement { return YES; }
- (NSString *)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString *)accessibilityIdentifier { return @"asb-role-catalog"; }
- (NSArray *)accessibilityChildren { return self.items; }
@end

NSView *ASBAdvancedFixture(NSRect frame) {
    ASBAdvancedFixtureView *view = [[ASBAdvancedFixtureView alloc] initWithFrame:frame];
    NSMutableArray *items = NSMutableArray.array;
    for (NSUInteger i = 0; i < 3; i++) {
        NSTextField *label = [NSTextField labelWithString:[NSString stringWithFormat:@"Heading %lu", (unsigned long)i + 1]];
        if (@available(macOS 26.0, *)) label.accessibilityRole = NSAccessibilityHeadingRole;
        label.accessibilityIdentifier = [NSString stringWithFormat:@"asb-heading-%lu", (unsigned long)i];
        label.frame = NSMakeRect(20, 300 - i * 60, 400, 30); [view addSubview:label]; [items addObject:label];
    }
    view.items = NSAccessibilityUnignoredChildren(items);
    AXCustomContent *high = [AXCustomContent customContentWithLabel:@"Priority detail" value:@"Always announced"];
    high.importance = AXCustomContentImportanceHigh;
    view.accessibilityCustomContent = @[[AXCustomContent customContentWithLabel:@"Fixture detail" value:@"Additional content"], high];
    return view;
}

NSDictionary *ASBConformanceCatalog(void) {
    static NSDictionary *catalog; static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *path = [NSBundle.mainBundle pathForResource:@"macos-sdk" ofType:@"json" inDirectory:@"AXConformance"];
        if (!path) path = [NSBundle.mainBundle pathForResource:@"macos-sdk" ofType:@"json"];
        catalog = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] ?: [NSData data] options:0 error:nil] ?: @{};
    });
    return catalog;
}

NSView *ASBRoleCatalogFixture(NSRect frame) {
    NSDictionary *catalog = ASBConformanceCatalog();
    ASBRoleCatalogView *view = [[ASBRoleCatalogView alloc] initWithFrame:frame];
    NSMutableArray *items = NSMutableArray.array; NSMutableSet *seen = NSMutableSet.set;
    for (NSDictionary *entry in catalog[@"entries"]) {
        NSString *kind = entry[@"kind"], *name = entry[@"axName"];
        NSString *key = [NSString stringWithFormat:@"%@-%@", kind, name];
        if (!name || ![@[@"role", @"subrole"] containsObject:kind] || [seen containsObject:key]) continue;
        [seen addObject:key];
        ASBCatalogElement *item = [[ASBCatalogElement alloc] init]; item.ownerView = view; item.row = items.count;
        item.accessibilityRole = [kind isEqual:@"role"] ? name : NSAccessibilityGroupRole;
        if ([kind isEqual:@"subrole"]) item.accessibilitySubrole = name;
        item.accessibilityIdentifier = [@"asb-catalog-" stringByAppendingString:key]; item.accessibilityLabel = name;
        item.accessibilityParent = view; [items addObject:item];
    }
    view.items = items; return view;
}
