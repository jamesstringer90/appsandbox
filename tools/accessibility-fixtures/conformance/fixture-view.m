#import <Cocoa/Cocoa.h>
#import "fixture-view.h"
#import "fixture-pages.h"
#import "fixture-advanced.h"
@interface ContractElement : NSObject
@property(weak) id accessibilityParent;
@property NSRect accessibilityFrame;
@property NSMutableDictionary *values;
@property NSMutableArray *items;
@property NSInteger presses;
@property NSArray *standardActions;
@property NSSet *writable;
@property NSUInteger mutationCount;
@property NSString *notificationName;
- (void)reset;
- (void)configure:(NSDictionary *)configuration;
@end
@implementation ContractElement
- (BOOL)accessibilityIsIgnored { return NO; }
- (BOOL)accessibilityNotifiesWhenDestroyed { return YES; }
- (void)dealloc { NSAccessibilityPostNotification(self, NSAccessibilityUIElementDestroyedNotification); }
- (instancetype)init {
    if ((self = [super init])) {
        self.items = NSMutableArray.array;
        NSMutableOrderedSet *actions = NSMutableOrderedSet.orderedSet;
        for (NSDictionary *entry in ASBConformanceCatalog()[@"entries"]) if ([entry[@"kind"] isEqual:@"action"] && entry[@"axName"]) [actions addObject:entry[@"axName"]];
        self.standardActions = actions.array; [self reset];
    }
    return self;
}
- (void)reset {
    self.presses = 0;
    self.mutationCount = 0;
    self.notificationName = nil;
    self.writable = [NSSet setWithArray:@[@"AXASBBoolean", @"AXASBInteger", @"AXASBDouble", @"AXASBUnicode", @"AXValue"]];
    NSAttributedString *attributed = [[NSAttributedString alloc] initWithString:@"alpha β😀" attributes:@{NSFontAttributeName: [NSFont fontWithName:@"Helvetica" size:14], NSForegroundColorAttributeName: NSColor.blueColor}];
    self.values = [@{@"AXASBBoolean": @YES, @"AXASBInteger": @42, @"AXASBDouble": @3.25,
        @"AXASBEmpty": @"", @"AXASBUnicode": @"café β 😀", @"AXASBArray": @[@1, @2, @3],
        @"AXASBDictionary": @{@"label": @"nested", @"values": @[@YES, @7]},
        @"AXASBData": [@"binary\0payload" dataUsingEncoding:NSUTF8StringEncoding],
        @"AXASBDate": [NSDate dateWithTimeIntervalSince1970:1700000000],
        @"AXASBURL": [NSURL URLWithString:@"https://example.invalid/ax?value=1"],
        @"AXASBPoint": [NSValue valueWithPoint:NSMakePoint(12, 34)],
        @"AXASBSize": [NSValue valueWithSize:NSMakeSize(56, 78)],
        @"AXASBRect": [NSValue valueWithRect:NSMakeRect(12, 34, 56, 78)],
        @"AXASBRange": [NSValue valueWithRange:NSMakeRange(2, 4)],
        @"AXASBAttributed": attributed, @"AXValue": @"ready"} mutableCopy];
}
- (id)decodeFixtureValue:(NSDictionary *)spec {
    NSString *type = spec[@"type"]; id value = spec[@"value"];
    if ([type isEqual:@"string"] && [value isKindOfClass:NSString.class]) return value;
    if ([type isEqual:@"boolean"]) return [value boolValue] ? @YES : @NO;
    if ([type isEqual:@"integer"]) return @([value longLongValue]);
    if ([type isEqual:@"double"]) return @([value doubleValue]);
    if ([type isEqual:@"data"]) return [[NSData alloc] initWithBase64EncodedString:value options:0];
    if ([type isEqual:@"date"]) return [NSDate dateWithTimeIntervalSince1970:[value doubleValue]];
    if ([type isEqual:@"url"]) return [NSURL URLWithString:value];
    if ([type isEqual:@"element"]) return [spec[@"child"] unsignedIntegerValue] < self.items.count ? self.items[[spec[@"child"] unsignedIntegerValue]] : nil;
    if ([type isEqual:@"range"] && [value count] == 2) return [NSValue valueWithRange:NSMakeRange([value[0] unsignedIntegerValue], [value[1] unsignedIntegerValue])];
    if ([type isEqual:@"point"] && [value count] == 2) return [NSValue valueWithPoint:NSMakePoint([value[0] doubleValue], [value[1] doubleValue])];
    if ([type isEqual:@"size"] && [value count] == 2) return [NSValue valueWithSize:NSMakeSize([value[0] doubleValue], [value[1] doubleValue])];
    if ([type isEqual:@"rect"] && [value count] == 4) return [NSValue valueWithRect:NSMakeRect([value[0] doubleValue], [value[1] doubleValue], [value[2] doubleValue], [value[3] doubleValue])];
    if ([type isEqual:@"array"]) {
        NSMutableArray *items = NSMutableArray.array;
        for (NSDictionary *item in value) { id decoded = [self decodeFixtureValue:item]; if (!decoded) return nil; [items addObject:decoded]; }
        return items;
    }
    if ([type isEqual:@"dictionary"]) {
        NSMutableDictionary *items = NSMutableDictionary.dictionary;
        for (NSString *key in value) { id decoded = [self decodeFixtureValue:value[key]]; if (!decoded) return nil; items[key] = decoded; }
        return items;
    }
    if ([type isEqual:@"attributed-string"]) return [[NSAttributedString alloc] initWithString:value attributes:@{NSFontAttributeName:[NSFont fontWithName:@"Helvetica" size:15]}];
    return nil;
}
- (void)configure:(NSDictionary *)configuration {
    if (![configuration isKindOfClass:NSDictionary.class]) return;
    if ([configuration[@"notification"] isKindOfClass:NSString.class]) self.notificationName = configuration[@"notification"];
    for (NSString *name in configuration[@"attributes"]) {
        id value = [self decodeFixtureValue:configuration[@"attributes"][name]];
        self.values[name] = value ?: NSNull.null;
    }
    if ([configuration[@"writable"] isKindOfClass:NSArray.class]) self.writable = [self.writable setByAddingObjectsFromArray:configuration[@"writable"]];
}
- (BOOL)isAccessibilityElement { return YES; }
- (NSArray *)accessibilityAttributeNames {
    NSMutableOrderedSet *names = [NSMutableOrderedSet orderedSetWithArray:self.values.allKeys];
    [names addObjectsFromArray:@[@"AXRole", @"AXSubrole", @"AXRoleDescription", @"AXIdentifier", @"AXTitle", @"AXDescription", @"AXParent", @"AXChildren", @"AXEnabled", @"AXPosition", @"AXSize", @"AXASBNoValue", @"AXASBReferences"]];
    return names.array;
}
- (id)accessibilityAttributeValue:(NSString *)name {
    if ([name isEqual:@"AXRole"]) return self.values[name] ?: NSAccessibilityGroupRole;
    if ([name isEqual:@"AXIdentifier"]) return @"asb-contract";
    if ([name isEqual:@"AXTitle"] || [name isEqual:@"AXDescription"]) return @"AX contract values";
    if ([name isEqual:@"AXRoleDescription"]) return @"group";
    if ([name isEqual:@"AXParent"]) return self.accessibilityParent;
    if ([name isEqual:@"AXChildren"]) return self.items;
    if ([name isEqual:@"AXASBReferences"]) return self.items.count ? @[self.items[0], self.items[0]] : @[];
    if ([name isEqual:@"AXEnabled"]) return @YES;
    if ([name isEqual:@"AXPosition"]) return [NSValue valueWithPoint:self.accessibilityFrame.origin];
    if ([name isEqual:@"AXSize"]) return [NSValue valueWithSize:self.accessibilityFrame.size];
    if ([name isEqual:@"AXASBNoValue"] || [name isEqual:@"AXSubrole"]) return nil;
    if (self.values[name]) return self.values[name] == NSNull.null ? nil : self.values[name];
    NSAccessibilityRaiseBadArgumentException(self, name, nil);
    return nil;
}
- (BOOL)accessibilityIsAttributeSettable:(NSString *)name {
    return [self.writable containsObject:name];
}
- (void)accessibilitySetValue:(id)value forAttribute:(NSString *)name {
    id previous = self.values[name]; BOOL compatible = NO;
    for (Class family in @[NSString.class, NSNumber.class, NSArray.class, NSDictionary.class, NSData.class, NSDate.class, NSURL.class, NSAttributedString.class, NSValue.class])
        if ([previous isKindOfClass:family]) { compatible = [value isKindOfClass:family]; break; }
    if ([previous isKindOfClass:NSNumber.class] && compatible)
        compatible = (CFGetTypeID((__bridge CFTypeRef)previous) == CFBooleanGetTypeID()) == (CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID());
    if ([previous isKindOfClass:NSValue.class] && ![previous isKindOfClass:NSNumber.class] && compatible)
        compatible = strcmp([previous objCType], [value objCType]) == 0;
    if (![self accessibilityIsAttributeSettable:name] || !compatible) { NSAccessibilityRaiseBadArgumentException(self, name, value); return; }
    self.values[name] = value;
    self.mutationCount++;
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}
- (NSArray *)accessibilityActionNames { return self.standardActions; }
- (NSString *)accessibilityActionDescription:(NSString *)action { return [self.accessibilityActionNames containsObject:action] ? action : nil; }
- (void)accessibilityPerformAction:(NSString *)action {
    if ([action isEqual:@"AXPress"] && self.notificationName) {
        NSString *name = self.notificationName;
        if ([name isEqual:NSAccessibilityAnnouncementRequestedNotification])
            NSAccessibilityPostNotificationWithUserInfo(self, name, @{NSAccessibilityAnnouncementKey:@"Fixture announcement", NSAccessibilityPriorityKey:@(NSAccessibilityPriorityHigh)});
        else NSAccessibilityPostNotification(self, name);
        return;
    } else if ([action isEqual:@"AXPress"]) self.values[@"AXValue"] = [NSString stringWithFormat:@"press:%ld", (long)++self.presses];
    else if ([self.standardActions containsObject:action]) {
        self.values[@"AXValue"] = action;
    } else { NSAccessibilityRaiseBadArgumentException(self, action, nil); return; }
    NSAccessibilityPostNotification(self, NSAccessibilityValueChangedNotification);
}
- (NSArray *)accessibilityParameterizedAttributeNames { return @[@"AXASBEcho", @"AXASBArrayItem"]; }
- (id)accessibilityAttributeValue:(NSString *)name forParameter:(id)parameter {
    if ([name isEqual:@"AXASBEcho"]) return parameter;
    if ([name isEqual:@"AXASBArrayItem"] && [parameter isKindOfClass:NSNumber.class] && [parameter integerValue] >= 0 && [parameter integerValue] < 3)
        return self.values[@"AXASBArray"][[parameter unsignedIntegerValue]];
    NSAccessibilityRaiseBadArgumentException(self, name, parameter); return nil;
}
@end

@interface ASBFixtureStateElement : NSAccessibilityElement
@property(weak) ASBFixtureView *fixture;
@end
@implementation ASBFixtureStateElement
- (BOOL)isAccessibilityElement { return YES; }
- (NSArray *)accessibilityAttributeNames { return @[@"AXRole", @"AXIdentifier", @"AXValue", @"AXParent", @"AXEnabled", @"AXPosition", @"AXSize"]; }
- (id)accessibilityAttributeValue:(NSString *)name {
    if ([name isEqual:@"AXRole"]) return NSAccessibilityStaticTextRole;
    if ([name isEqual:@"AXIdentifier"]) return @"asb-state";
    if ([name isEqual:@"AXValue"]) return [self.fixture stateJSON];
    if ([name isEqual:@"AXParent"]) return self.fixture;
    if ([name isEqual:@"AXEnabled"]) return @YES;
    NSRect frame = NSAccessibilityFrameInView(self.fixture.stateField, self.fixture.stateField.bounds);
    if ([name isEqual:@"AXPosition"]) return [NSValue valueWithPoint:frame.origin];
    if ([name isEqual:@"AXSize"]) return [NSValue valueWithSize:frame.size];
    return [super accessibilityAttributeValue:name];
}
- (BOOL)accessibilityIsAttributeSettable:(NSString *)name { (void)name; return NO; }
@end
@interface ASBFixtureView ()
@property NSDictionary *lastState;
@property ASBFixtureStateElement *stateElement;
@property NSPopUpButton *pagePicker;
@property NSView *pageContainer, *basics;
@property NSMutableDictionary<NSString *, ASBFixturePage *> *pages;
@property ContractElement *contract;
@end

@implementation ASBFixtureView
- (BOOL)isAccessibilityElement { return YES; }
- (NSString *)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString *)accessibilityIdentifier { return @"asb-fixture"; }
- (NSString *)accessibilityLabel { return @"Accessibility conformance fixture"; }
- (NSButton *)button:(NSString *)title identifier:(NSString *)identifier action:(SEL)action y:(CGFloat)y {
    NSButton *button = [NSButton buttonWithTitle:title target:self action:action];
    button.frame = NSMakeRect(20, y, 240, 30); button.accessibilityIdentifier = identifier; [self addSubview:button]; return button;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.resetting = YES;
    NSTextField *heading = [NSTextField labelWithString:@"Native accessibility test fixture"];
    heading.font = [NSFont boldSystemFontOfSize:17]; heading.frame = NSMakeRect(20, 610, 500, 26); [self addSubview:heading];
    self.caseField = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 570, 290, 27)];
    self.caseField.accessibilityIdentifier = @"asb-case"; self.caseField.accessibilityLabel = @"Case ID"; self.caseField.stringValue = @"initial"; [self addSubview:self.caseField];
    NSButton *reset = [self button:@"Reset case" identifier:@"asb-reset" action:@selector(reset:) y:570]; reset.frame = NSMakeRect(330, 570, 160, 27);
    [self button:@"Count press" identifier:@"asb-button" action:@selector(press:) y:525];
    NSButton *disabled = [self button:@"Disabled button" identifier:@"asb-disabled" action:@selector(press:) y:485]; disabled.enabled = NO;
    self.checkbox = [NSButton checkboxWithTitle:@"Meal included" target:self action:@selector(changed:)];
    self.checkbox.frame = NSMakeRect(280, 525, 210, 30); self.checkbox.accessibilityIdentifier = @"asb-checkbox"; [self addSubview:self.checkbox];
    NSSlider *slider = [[NSSlider alloc] initWithFrame:NSMakeRect(20, 440, 360, 28)];
    slider.minValue = 0; slider.maxValue = 10; slider.accessibilityLabel = @"Volume"; slider.accessibilityIdentifier = @"asb-slider";
    slider.target = self; slider.action = @selector(changed:); self.slider = slider; [self addSubview:slider];
    NSTextField *text = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 395, 470, 28)];
    text.accessibilityLabel = @"Report title"; text.accessibilityIdentifier = @"asb-text"; text.delegate = self; self.textField = text; [self addSubview:text];
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 220, 470, 155)]; scroll.hasVerticalScroller = YES; scroll.borderType = NSBezelBorder;
    self.editor = [[NSTextView alloc] initWithFrame:scroll.bounds]; self.editor.richText = YES; self.editor.font = [NSFont systemFontOfSize:15];
    self.editor.textColor = NSColor.blackColor; self.editor.backgroundColor = NSColor.whiteColor;
    self.editor.accessibilityIdentifier = @"asb-editor"; self.editor.accessibilityLabel = @"Trip notes"; self.editor.delegate = self;
    scroll.documentView = self.editor; [self addSubview:scroll];
    self.stateField = [NSTextField wrappingLabelWithString:@""]; self.stateField.frame = NSMakeRect(20, 110, 470, 95);
    self.stateField.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]; [self addSubview:self.stateField];
    self.stateElement = [[ASBFixtureStateElement alloc] init]; self.stateElement.fixture = self; self.stateElement.accessibilityParent = self;
    ContractElement *contract = [[ContractElement alloc] init]; self.contract = contract; contract.accessibilityParent = self; contract.accessibilityFrame = NSMakeRect(20, 20, 470, 70);
    for (int i = 0; i < 3; i++) {
        NSAccessibilityElement *child = [[NSAccessibilityElement alloc] init]; child.accessibilityRole = NSAccessibilityStaticTextRole;
        child.accessibilityLabel = [NSString stringWithFormat:@"Contract item %d", i]; child.accessibilityIdentifier = [NSString stringWithFormat:@"asb-item-%d", i];
        child.accessibilityParent = contract; child.accessibilityFrame = contract.accessibilityFrame; [contract.items addObject:child];
    }
    self.basics = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 510, 440)];
    for (NSView *control in self.subviews.copy) {
        if (control != self.stateField && control.frame.origin.y < 570) {
            NSRect bounds = control.frame; bounds.origin.y -= 140; [self.basics addSubview:control]; control.frame = bounds;
        }
    }
    self.stateField.frame = NSMakeRect(20, 5, 470, 78);
    self.pagePicker = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(20, 530, 290, 28)]; self.pagePicker.accessibilityIdentifier = @"asb-page-picker";
    [self.pagePicker addItemsWithTitles:@[@"Basics", @"Choices", @"Text", @"Table", @"Outline", @"Navigation", @"Geometry", @"Events", @"Advanced", @"Web", @"Role catalog", @"Stress"]];
    self.pagePicker.target = self; self.pagePicker.action = @selector(changePage:); [self addSubview:self.pagePicker];
    self.pageContainer = [[NSView alloc] initWithFrame:NSMakeRect(0, 85, 510, 440)]; [self addSubview:self.pageContainer];
    self.pages = NSMutableDictionary.dictionary;
    for (NSString *name in self.pagePicker.itemTitles) if (![name isEqual:@"Basics"]) self.pages[name] = [[ASBFixturePage alloc] initWithName:name fixture:self];
    [self changePage:nil];
    [self reset:nil];
    return self;
}
- (void)reset:(id)sender {
    (void)sender; self.resetting = YES; self.presses = 0; self.revision = 0; self.lastState = nil;
    NSDictionary *configuration = [NSJSONSerialization JSONObjectWithData:[self.caseField.stringValue dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    if (![configuration isKindOfClass:NSDictionary.class]) configuration = nil;
    NSString *page = configuration[@"page"] ?: [self.caseField.stringValue componentsSeparatedByString:@":"].lastObject;
    if ([self.pagePicker.itemTitles containsObject:page]) { [self.pagePicker selectItemWithTitle:page]; [self changePage:nil]; }
    self.checkbox.state = NSControlStateValueOff; self.slider.doubleValue = 4; self.textField.stringValue = @"initial";
    self.editor.string = @"Plan a quiet morning.\nUnicode: café β 😀."; self.editor.selectedRange = NSMakeRange(0, 0);
    [self.contract reset]; [self.contract configure:configuration[@"provider"]]; for (ASBFixturePage *page in self.pages.allValues) [page reset];
    if ([page isEqual:@"Stress"]) [self.pages[page] configureStress:configuration[@"stress"]];
    self.resetting = NO; [self publishState];
}
- (NSArray *)accessibilityChildren {
    NSMutableArray *controls = [self.subviews mutableCopy]; [controls removeObject:self.stateField];
    return [NSAccessibilityUnignoredChildren(controls) arrayByAddingObjectsFromArray:@[self.stateElement, self.contract]];
}
- (void)changePage:(id)sender {
    (void)sender; for (NSView *view in self.pageContainer.subviews.copy) [view removeFromSuperview];
    NSString *name = self.pagePicker.titleOfSelectedItem;
    [self.pageContainer addSubview:[name isEqual:@"Basics"] ? self.basics : self.pages[name].view];
    if (!self.resetting) { NSAccessibilityPostNotification(self, NSAccessibilityLayoutChangedNotification); [self publishState]; }
}
- (void)publishState {
    if (self.resetting || !self.stateField) return;
    [self stateJSON];
    NSAccessibilityPostNotification(self.stateElement, NSAccessibilityValueChangedNotification);
}
- (NSString *)stateJSON {
    NSMutableDictionary *state = [@{@"case": self.caseField.stringValue ?: @"", @"count": @(self.presses), @"page": self.pagePicker.titleOfSelectedItem ?: @"Basics",
        @"checked": self.checkbox.state == NSControlStateValueOn ? @YES : @NO, @"range": @(self.slider.doubleValue),
        @"text": self.textField.stringValue ?: @"", @"selection": @[@(self.editor.selectedRange.location), @(self.editor.selectedRange.length)]} mutableCopy];
    for (ASBFixturePage *page in self.pages.allValues) [state addEntriesFromDictionary:page.state];
    state[@"fixtureRevision"] = [NSBundle.mainBundle objectForInfoDictionaryKey:@"ASBAXFixtureRevision"] ?: @"missing";
    state[@"contractValue"] = self.contract.values[@"AXValue"] ?: @"";
    state[@"contractBoolean"] = self.contract.values[@"AXASBBoolean"] ?: @NO;
    state[@"contractInteger"] = self.contract.values[@"AXASBInteger"] ?: @0;
    state[@"contractDouble"] = self.contract.values[@"AXASBDouble"] ?: @0;
    state[@"contractUnicode"] = self.contract.values[@"AXASBUnicode"] ?: @"";
    state[@"contractMutations"] = @(self.contract.mutationCount);
    if (![state isEqual:self.lastState]) { self.revision++; self.lastState = state; }
    NSMutableDictionary *versioned = [state mutableCopy]; versioned[@"revision"] = @(self.revision);
    NSData *json = [NSJSONSerialization dataWithJSONObject:versioned options:NSJSONWritingSortedKeys error:nil];
    self.stateField.stringValue = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
    return self.stateField.stringValue;
}
- (void)press:(id)sender { (void)sender; self.presses++; [self publishState]; }
- (void)changed:(id)sender { (void)sender; [self publishState]; }
- (void)controlTextDidChange:(NSNotification *)notification { (void)notification; [self publishState]; }
- (void)textDidChange:(NSNotification *)notification { (void)notification; [self publishState]; }
- (void)textViewDidChangeSelection:(NSNotification *)notification { (void)notification; [self publishState]; }
@end
