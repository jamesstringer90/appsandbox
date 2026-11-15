#import "fixture-pages.h"
#import "fixture-view.h"
#import "fixture-advanced.h"
#import "fixture-stress.h"

@interface ASBFixturePage ()
@property NSView *view;
@property NSString *name;
@property(weak) ASBFixtureView *fixture;
@property NSMutableDictionary<NSString *, NSView *> *controls;
@property NSTableView *table;
@property NSOutlineView *outline;
@property NSArray *groups;
@property NSTextView *editor;
@property NSTabView *tabs;
@property NSInteger delayed, generation;
@property WKWebView *web;
@property BOOL webReady;
@property ASBStressFixtureView *stress;
@end

@implementation ASBFixturePage
- (NSView *)add:(NSView *)control name:(NSString *)name frame:(NSRect)frame {
    control.frame = frame; control.accessibilityIdentifier = [@"asb-" stringByAppendingString:name];
    control.accessibilityLabel = name;
    self.controls[name] = control; [self.view addSubview:control]; return control;
}
- (NSButton *)button:(NSString *)title name:(NSString *)name y:(CGFloat)y action:(SEL)action {
    return (NSButton *)[self add:[NSButton buttonWithTitle:title target:self action:action] name:name frame:NSMakeRect(20, y, 220, 28)];
}
- (NSScrollView *)scroll:(NSString *)name frame:(NSRect)frame {
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = YES; scroll.borderType = NSBezelBorder;
    [self add:scroll name:name frame:frame]; return scroll;
}
- (instancetype)initWithName:(NSString *)name fixture:(ASBFixtureView *)fixture {
    if (!(self = [super init])) return nil;
    self.name = name; self.fixture = fixture; self.controls = NSMutableDictionary.dictionary;
    self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 510, 440)];
    if ([name isEqual:@"Stress"]) {
        self.stress = [[ASBStressFixtureView alloc] initWithFrame:self.view.bounds]; [self.view addSubview:self.stress];
    } else if ([name isEqual:@"Web"]) {
        self.web = [[WKWebView alloc] initWithFrame:self.view.bounds]; self.web.navigationDelegate = self;
        self.web.accessibilityIdentifier = @"asb-web"; [self.view addSubview:self.web];
        NSString *path = [NSBundle.mainBundle pathForResource:@"web-fixture" ofType:@"html" inDirectory:@"AXConformance"];
        if (!path) path = [NSBundle.mainBundle pathForResource:@"web-fixture" ofType:@"html"];
        NSString *html = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        [self.web loadHTMLString:html ?: @"Missing web fixture" baseURL:nil];
    } else if ([name isEqual:@"Advanced"]) {
        [self.view addSubview:ASBAdvancedFixture(self.view.bounds)];
    } else if ([name isEqual:@"Role catalog"]) {
        [self.view addSubview:ASBRoleCatalogFixture(self.view.bounds)];
    } else if ([name isEqual:@"Choices"]) {
        NSButton *mixed = [NSButton checkboxWithTitle:@"Mixed selection" target:self action:@selector(changed:)]; mixed.allowsMixedState = YES;
        [self add:mixed name:@"mixed" frame:NSMakeRect(20, 390, 230, 28)];
        NSSwitch *toggle = [[NSSwitch alloc] init]; toggle.target = self; toggle.action = @selector(changed:);
        [self add:toggle name:@"switch" frame:NSMakeRect(280, 390, 70, 28)];
        for (NSUInteger i = 0; i < 2; i++) {
            NSButton *radio = [NSButton radioButtonWithTitle:i ? @"Aisle seat" : @"Window seat" target:self action:@selector(radio:)]; radio.tag = i;
            [self add:radio name:i ? @"radio-aisle" : @"radio-window" frame:NSMakeRect(20 + i * 240, 345, 220, 28)];
        }
        NSStepper *stepper = [[NSStepper alloc] init]; stepper.minValue = 1; stepper.maxValue = 9; stepper.valueWraps = NO;
        stepper.target = self; stepper.action = @selector(changed:); [self add:stepper name:@"stepper" frame:NSMakeRect(20, 290, 24, 32)];
        NSPopUpButton *popup = [[NSPopUpButton alloc] init]; [popup addItemsWithTitles:@[@"Gore", @"Queenstown", @"Dunedin"]]; popup.target = self; popup.action = @selector(changed:);
        [self add:popup name:@"popup" frame:NSMakeRect(80, 290, 200, 32)];
        NSComboBox *combo = [[NSComboBox alloc] init]; [combo addItemsWithObjectValues:@[@"Gallery", @"Fishing", @"Museum"]]; combo.target = self; combo.action = @selector(changed:);
        [self add:combo name:@"combo" frame:NSMakeRect(20, 240, 220, 28)];
        NSSegmentedControl *segments = [NSSegmentedControl segmentedControlWithLabels:@[@"Morning", @"Afternoon", @"Evening"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(changed:)];
        [self add:segments name:@"segments" frame:NSMakeRect(20, 190, 420, 28)];
        NSDatePicker *date = [[NSDatePicker alloc] init]; date.datePickerStyle = NSDatePickerStyleTextFieldAndStepper; date.datePickerElements = NSDatePickerElementFlagYearMonthDay;
        [self add:date name:@"date" frame:NSMakeRect(20, 140, 230, 28)];
        NSProgressIndicator *progress = [[NSProgressIndicator alloc] init]; progress.indeterminate = NO; progress.minValue = 0; progress.maxValue = 100; progress.doubleValue = 65;
        [self add:progress name:@"progress" frame:NSMakeRect(20, 90, 220, 22)];
        NSLevelIndicator *level = [[NSLevelIndicator alloc] init]; level.minValue = 0; level.maxValue = 100; level.doubleValue = 40;
        [self add:level name:@"level" frame:NSMakeRect(260, 90, 220, 22)];
        NSColorWell *color = [[NSColorWell alloc] init]; color.color = NSColor.blueColor;
        [self add:color name:@"color" frame:NSMakeRect(20, 30, 70, 30)];
    } else if ([name isEqual:@"Text"]) {
        NSSearchField *search = [[NSSearchField alloc] init]; [self add:search name:@"search" frame:NSMakeRect(20, 395, 220, 28)];
        NSSecureTextField *secure = [[NSSecureTextField alloc] init]; [self add:secure name:@"secure" frame:NSMakeRect(260, 395, 220, 28)];
        NSScrollView *scroll = [self scroll:@"rich-scroll" frame:NSMakeRect(20, 110, 460, 260)];
        self.editor = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 440, 260)]; self.editor.richText = YES;
        self.editor.font = [NSFont fontWithName:@"Helvetica" size:15]; self.editor.textColor = NSColor.blackColor; self.editor.backgroundColor = NSColor.whiteColor;
        self.editor.accessibilityIdentifier = @"asb-richtext"; self.editor.delegate = self;
        self.editor.automaticQuoteSubstitutionEnabled = NO; self.editor.automaticTextReplacementEnabled = NO; self.editor.automaticSpellingCorrectionEnabled = NO;
        scroll.documentView = self.editor;
        [self button:@"Bold selection" name:@"bold" y:65 action:@selector(bold:)];
        [self button:@"Italic selection" name:@"italic" y:25 action:@selector(italic:)];
    } else if ([name isEqual:@"Table"]) {
        NSScrollView *scroll = [self scroll:@"table-scroll" frame:NSMakeRect(20, 35, 470, 380)];
        self.table = [[NSTableView alloc] initWithFrame:scroll.bounds]; self.table.accessibilityIdentifier = @"asb-table";
        self.table.rowHeight = 26; self.table.allowsMultipleSelection = YES; self.table.dataSource = self; self.table.delegate = self;
        for (NSArray *spec in @[@[@"name", @"Reservation", @230], @[@"city", @"City", @150], @[@"amount", @"Amount", @100]]) {
            NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:spec[0]]; column.title = spec[1]; column.width = [spec[2] doubleValue]; column.editable = NO;
            [self.table addTableColumn:column];
        }
        scroll.documentView = self.table;
    } else if ([name isEqual:@"Outline"]) {
        NSMutableArray *groups = NSMutableArray.array;
        for (int i = 0; i < 30; i++) {
            NSMutableArray *children = NSMutableArray.array;
            for (int j = 0; j < 3; j++) [children addObject:[NSString stringWithFormat:@"Day %02d activity %d", i + 1, j + 1]];
            [groups addObject:@{@"name": [NSString stringWithFormat:@"Day %02d", i + 1], @"children": children}];
        }
        self.groups = groups;
        NSScrollView *scroll = [self scroll:@"outline-scroll" frame:NSMakeRect(20, 35, 470, 380)];
        self.outline = [[NSOutlineView alloc] initWithFrame:scroll.bounds]; self.outline.accessibilityIdentifier = @"asb-outline";
        self.outline.rowHeight = 26; self.outline.allowsMultipleSelection = YES; self.outline.dataSource = self; self.outline.delegate = self;
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"name"]; column.title = @"Itinerary"; column.width = 430;
        [self.outline addTableColumn:column]; self.outline.outlineTableColumn = column; scroll.documentView = self.outline;
    } else if ([name isEqual:@"Navigation"]) {
        self.tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(20, 190, 470, 220)];
        for (NSString *title in @[@"Overview", @"Details", @"History"]) {
            NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:title]; item.label = title;
            NSTextField *label = [NSTextField labelWithString:[title stringByAppendingString:@" content"]];
            label.frame = NSMakeRect(20, 40, 300, 30); label.accessibilityIdentifier = [@"asb-tab-" stringByAppendingString:title.lowercaseString];
            item.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 440, 170)]; [item.view addSubview:label]; [self.tabs addTabViewItem:item];
        }
        [self add:self.tabs name:@"tabs" frame:self.tabs.frame];
        NSBox *group = [[NSBox alloc] initWithFrame:NSMakeRect(20, 80, 220, 90)]; group.title = @"Labelled group";
        [self add:group name:@"labelled-group" frame:group.frame];
        NSImageView *image = [[NSImageView alloc] initWithFrame:NSMakeRect(280, 80, 60, 60)]; image.image = [NSImage imageNamed:NSImageNameInfo];
        [self add:image name:@"image" frame:image.frame]; image.accessibilityLabel = @"Information image";
        NSButton *link = [NSButton buttonWithTitle:@"Local link action" target:self action:@selector(delayed:)];
        link.accessibilityRole = NSAccessibilityLinkRole; [self add:link name:@"link" frame:NSMakeRect(20, 20, 220, 28)];
    } else if ([name isEqual:@"Geometry"]) {
        NSScrollView *scroll = [self scroll:@"nested-scroll" frame:NSMakeRect(20, 35, 470, 380)];
        NSView *document = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 900, 1800)];
        for (int i = 0; i < 40; i++) {
            NSButton *button = [NSButton buttonWithTitle:[NSString stringWithFormat:@"Scroll target %02d", i] target:self action:@selector(changed:)];
            button.frame = NSMakeRect(i % 2 ? 600 : 20, 1750 - i * 43, 250, 28);
            button.accessibilityIdentifier = [NSString stringWithFormat:@"asb-scroll-%02d", i]; [document addSubview:button];
        }
        scroll.documentView = document;
    } else if ([name isEqual:@"Events"]) {
        [self button:@"Delayed state update" name:@"delayed" y:360 action:@selector(delayed:)];
        [self button:@"Open sheet" name:@"sheet-open" y:310 action:@selector(sheet:)];
        [self button:@"Add dynamic control" name:@"dynamic-add" y:260 action:@selector(addDynamic:)];
        [self button:@"Remove dynamic control" name:@"dynamic-remove" y:210 action:@selector(removeDynamic:)];
        NSTextField *status = [NSTextField labelWithString:@"idle"];
        [self add:status name:@"event-state" frame:NSMakeRect(20, 160, 450, 28)];
    }
    [self reset]; return self;
}
- (NSArray *)indexes:(NSIndexSet *)indexes {
    NSMutableArray *result = NSMutableArray.array;
    [indexes enumerateIndexesUsingBlock:^(NSUInteger index, BOOL *stop) { (void)stop; [result addObject:@(index)]; }]; return result;
}
- (void)reset {
    [self.stress stop:nil];
    self.generation++; self.delayed = 0;
    if (self.webReady) [self.web evaluateJavaScript:@"resetFixture()" completionHandler:nil];
    [(NSButton *)self.controls[@"mixed"] setState:NSControlStateValueMixed];
    [(NSSwitch *)self.controls[@"switch"] setState:NSControlStateValueOff];
    [(NSButton *)self.controls[@"radio-window"] setState:NSControlStateValueOn]; [(NSButton *)self.controls[@"radio-aisle"] setState:NSControlStateValueOff];
    [(NSStepper *)self.controls[@"stepper"] setIntegerValue:2];
    [(NSPopUpButton *)self.controls[@"popup"] selectItemAtIndex:0]; [(NSComboBox *)self.controls[@"combo"] setStringValue:@"Gallery"];
    [(NSSegmentedControl *)self.controls[@"segments"] setSelectedSegment:0];
    [(NSDatePicker *)self.controls[@"date"] setDateValue:[NSDate dateWithTimeIntervalSince1970:1700000000]];
    [(NSSearchField *)self.controls[@"search"] setStringValue:@"initial search"]; [(NSSecureTextField *)self.controls[@"secure"] setStringValue:@"fixture-secret"];
    if (self.editor) {
        self.editor.string = @"First line: café β 😀.\nSecond line has a link.\nThird line for selection.";
        [self.editor.textStorage setAttributes:@{NSFontAttributeName: [NSFont fontWithName:@"Helvetica" size:15], NSForegroundColorAttributeName:NSColor.blackColor} range:NSMakeRange(0, self.editor.string.length)];
        self.editor.selectedRange = NSMakeRange(0, 0);
    }
    [self.table deselectAll:nil]; [self.outline collapseItem:nil collapseChildren:YES]; [self.outline deselectAll:nil]; [self.tabs selectTabViewItemAtIndex:0];
    [self removeDynamic:nil]; [(NSTextField *)self.controls[@"event-state"] setStringValue:@"idle"];
}
- (NSDictionary *)state {
    if (self.stress) return @{@"stress":self.stress.state};
    if (self.web) return @{@"webReady": self.webReady ? @YES : @NO};
    if ([self.name isEqual:@"Choices"]) return @{@"mixed": @([(NSButton *)self.controls[@"mixed"] state]), @"switch": @([(NSSwitch *)self.controls[@"switch"] state]),
        @"seat": [(NSButton *)self.controls[@"radio-aisle"] state] ? @"aisle" : @"window", @"stepper": @([(NSStepper *)self.controls[@"stepper"] integerValue]),
        @"popup": [(NSPopUpButton *)self.controls[@"popup"] titleOfSelectedItem], @"combo": [(NSComboBox *)self.controls[@"combo"] stringValue],
        @"segment": @([(NSSegmentedControl *)self.controls[@"segments"] selectedSegment])};
    if (self.editor) {
        NSMutableArray *bold = NSMutableArray.array, *italic = NSMutableArray.array;
        [self.editor.textStorage enumerateAttribute:NSFontAttributeName inRange:NSMakeRange(0, self.editor.string.length) options:0 usingBlock:^(NSFont *font, NSRange range, BOOL *stop) {
            (void)stop; NSFontTraitMask traits = [NSFontManager.sharedFontManager traitsOfFont:font];
            NSArray *span = @[@(range.location), @(range.length)];
            if (traits & NSBoldFontMask) [bold addObject:span]; if (traits & NSItalicFontMask) [italic addObject:span];
        }];
        return @{@"richText": self.editor.string, @"richSelection": @[@(self.editor.selectedRange.location), @(self.editor.selectedRange.length)],
            @"richBold": bold, @"richItalic": italic, @"search": [(NSSearchField *)self.controls[@"search"] stringValue]};
    }
    if (self.table) return @{@"tableSelection": [self indexes:self.table.selectedRowIndexes], @"tableRows": @(self.table.numberOfRows)};
    if (self.outline) {
        NSMutableArray *expanded = NSMutableArray.array;
        for (NSUInteger i = 0; i < self.groups.count; i++) if ([self.outline isItemExpanded:self.groups[i]]) [expanded addObject:@(i)];
        return @{@"outlineSelection": [self indexes:self.outline.selectedRowIndexes], @"expanded": expanded, @"outlineRows": @(self.outline.numberOfRows)};
    }
    if (self.tabs) return @{@"tab": self.tabs.selectedTabViewItem.identifier};
    if ([self.name isEqual:@"Events"]) return @{@"delayed": @(self.delayed), @"dynamic": self.controls[@"dynamic"] ? @YES : @NO};
    return @{};
}
- (void)configureStress:(NSDictionary *)configuration { [self.stress configure:configuration]; }
- (void)changed:(id)sender { (void)sender; [self.fixture publishState]; }
- (void)radio:(NSButton *)sender {
    [(NSButton *)self.controls[@"radio-window"] setState:sender.tag == 0]; [(NSButton *)self.controls[@"radio-aisle"] setState:sender.tag == 1]; [self changed:sender];
}
- (void)format:(NSFontTraitMask)trait {
    NSRange range = self.editor.selectedRange;
    [self.editor.textStorage applyFontTraits:trait range:range]; [self.fixture publishState];
    NSAccessibilityPostNotification(self.editor, NSAccessibilityValueChangedNotification);
}
- (void)bold:(id)sender { (void)sender; [self format:NSBoldFontMask]; }
- (void)italic:(id)sender { (void)sender; [self format:NSItalicFontMask]; }
- (void)delayed:(id)sender {
    (void)sender; NSInteger generation = self.generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (generation != self.generation) return;
        self.delayed++; [(NSTextField *)self.controls[@"event-state"] setStringValue:@"updated"];
        NSAccessibilityPostNotification(self.controls[@"event-state"] ?: self.view, NSAccessibilityValueChangedNotification); [self.fixture publishState];
    });
}
- (void)addDynamic:(id)sender {
    (void)sender; if (self.controls[@"dynamic"]) return;
    [self button:@"Dynamic child" name:@"dynamic" y:110 action:@selector(changed:)];
    NSAccessibilityPostNotification(self.view, NSAccessibilityLayoutChangedNotification); [self.fixture publishState];
}
- (void)removeDynamic:(id)sender {
    (void)sender; [self.controls[@"dynamic"] removeFromSuperview]; [self.controls removeObjectForKey:@"dynamic"];
    if (!self.fixture.resetting) { NSAccessibilityPostNotification(self.view, NSAccessibilityLayoutChangedNotification); [self.fixture publishState]; }
}
- (void)sheet:(id)sender {
    (void)sender; NSAlert *alert = [[NSAlert alloc] init]; alert.messageText = @"Fixture confirmation";
    [alert addButtonWithTitle:@"Confirm fixture"]; [alert addButtonWithTitle:@"Cancel"];
    [alert beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response) {
        [(NSTextField *)self.controls[@"event-state"] setStringValue:response == NSAlertFirstButtonReturn ? @"confirmed" : @"cancelled"]; [self.fixture publishState];
    }];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table { (void)table; return 120; }
- (id)tableView:(NSTableView *)table objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)table; if ([column.identifier isEqual:@"name"]) return [NSString stringWithFormat:@"Reservation %03ld", row + 1];
    if ([column.identifier isEqual:@"city"]) return @[@"Gore", @"Queenstown", @"Dunedin"][row % 3]; return @(row * 10 + 25);
}
- (NSInteger)outlineView:(NSOutlineView *)outline numberOfChildrenOfItem:(id)item { (void)outline; return !item ? self.groups.count : [item isKindOfClass:NSDictionary.class] ? [item[@"children"] count] : 0; }
- (id)outlineView:(NSOutlineView *)outline child:(NSInteger)index ofItem:(id)item { (void)outline; return item ? item[@"children"][index] : self.groups[index]; }
- (BOOL)outlineView:(NSOutlineView *)outline isItemExpandable:(id)item { (void)outline; return [item isKindOfClass:NSDictionary.class]; }
- (id)outlineView:(NSOutlineView *)outline objectValueForTableColumn:(NSTableColumn *)column byItem:(id)item { (void)outline; (void)column; return [item isKindOfClass:NSDictionary.class] ? item[@"name"] : item; }
- (void)tableViewSelectionDidChange:(NSNotification *)notification { (void)notification; [self.fixture publishState]; }
- (void)outlineViewSelectionDidChange:(NSNotification *)notification { (void)notification; [self.fixture publishState]; }
- (void)textDidChange:(NSNotification *)notification { (void)notification; [self.fixture publishState]; }
- (void)textViewDidChangeSelection:(NSNotification *)notification { (void)notification; [self.fixture publishState]; }
- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation { (void)webView; (void)navigation; self.webReady = YES; [self.fixture publishState]; }
@end
