#import <Cocoa/Cocoa.h>

@interface LabDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate, NSOutlineViewDataSource, NSOutlineViewDelegate, NSTextViewDelegate>
@property NSWindow *window;
@property NSTextField *status;
@property NSTableView *table;
@property NSOutlineView *outline;
@property NSArray *groups;
@property NSInteger count;
@property NSTextField *travelerValue;
@property NSTextField *volumeValue;
@property NSButton *windowSeat;
@property NSButton *aisleSeat;
@property BOOL background;
@end

@implementation LabDelegate
- (NSTextField *)label:(NSString *)text frame:(NSRect)frame in:(NSView *)view {
    NSTextField *label=[NSTextField labelWithString:text]; label.frame=frame; [view addSubview:label]; return label;
}
- (NSButton *)button:(NSString *)title frame:(NSRect)frame in:(NSView *)view action:(SEL)action {
    NSButton *button=[NSButton buttonWithTitle:title target:self action:action]; button.frame=frame; [view addSubview:button]; return button;
}
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    NSMenu *menu=[NSMenu new]; NSMenuItem *appItem=[NSMenuItem new]; [menu addItem:appItem];
    NSMenu *appMenu=[NSMenu new]; [appMenu addItemWithTitle:@"Quit Control Lab" action:@selector(terminate:) keyEquivalent:@"q"]; appItem.submenu=appMenu;
    NSMenuItem *editItem=[[NSMenuItem alloc]initWithTitle:@"Edit" action:nil keyEquivalent:@""]; [menu addItem:editItem]; NSMenu *edit=[NSMenu new]; edit.title=@"Edit"; editItem.submenu=edit;
    for (NSArray *item in @[@[@"Undo",@"undo:",@"z"],@[@"Cut",@"cut:",@"x"],@[@"Copy",@"copy:",@"c"],@[@"Paste",@"paste:",@"v"],@[@"Select All",@"selectAll:",@"a"]]) [edit addItemWithTitle:item[0] action:NSSelectorFromString(item[1]) keyEquivalent:item[2]];
    NSMenuItem *formatItem=[[NSMenuItem alloc]initWithTitle:@"Format" action:nil keyEquivalent:@""]; [menu addItem:formatItem]; NSMenu *format=[[NSMenu alloc]initWithTitle:@"Format"]; formatItem.submenu=format;
    NSMenuItem *fontItem=[[NSMenuItem alloc]initWithTitle:@"Font" action:nil keyEquivalent:@""]; [format addItem:fontItem]; fontItem.submenu=[[NSFontManager sharedFontManager]fontMenu:YES];
    NSApp.mainMenu=menu;
    self.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(180,100,1000,720) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    self.window.title=@"AppSandbox Native Control Lab";
    NSView *content=self.window.contentView;
    [self label:@"Native AppKit controls" frame:NSMakeRect(24,678,850,26) in:content].font=[NSFont boldSystemFontOfSize:22];
    self.status=[self label:@"Last action: none" frame:NSMakeRect(24,14,950,28) in:content]; self.status.accessibilityIdentifier=@"lab-status";
    NSTabView *tabs=[[NSTabView alloc]initWithFrame:NSMakeRect(20,55,960,610)]; tabs.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    [content addSubview:tabs];
    NSMutableArray *pages=NSMutableArray.array;
    for(NSString *name in @[@"Controls",@"Lists and outline",@"Text editing"]){NSTabViewItem *item=[[NSTabViewItem alloc]initWithIdentifier:name];item.label=name;NSView *view=[[NSView alloc]initWithFrame:NSMakeRect(0,0,930,555)];item.view=view;[tabs addTabViewItem:item];[pages addObject:view];}
    NSView *controls=pages[0];
    [self button:@"Count press" frame:NSMakeRect(24,490,150,34) in:controls action:@selector(countPress:)];
    NSButton *disabled=[self button:@"Unavailable" frame:NSMakeRect(190,490,150,34) in:controls action:@selector(changed:)];disabled.enabled=NO;
    NSButton *check=[NSButton checkboxWithTitle:@"Meal included" target:self action:@selector(changed:)];check.frame=NSMakeRect(24,436,190,30);[controls addSubview:check];
    NSSwitch *toggle=[[NSSwitch alloc]initWithFrame:NSMakeRect(245,436,54,30)];toggle.accessibilityLabel=@"Notifications";toggle.target=self;toggle.action=@selector(changed:);[controls addSubview:toggle];[self label:@"Notifications" frame:NSMakeRect(310,440,150,24) in:controls];
    NSButton *window=[NSButton radioButtonWithTitle:@"Window seat" target:self action:@selector(seatChanged:)];window.frame=NSMakeRect(490,438,150,26);window.state=NSControlStateValueOn;self.windowSeat=window;[controls addSubview:window];
    NSButton *aisle=[NSButton radioButtonWithTitle:@"Aisle seat" target:self action:@selector(seatChanged:)];aisle.frame=NSMakeRect(660,438,150,26);self.aisleSeat=aisle;[controls addSubview:aisle];
    [self label:@"Volume" frame:NSMakeRect(24,380,100,24) in:controls];NSSlider *slider=[NSSlider sliderWithValue:4 minValue:0 maxValue:10 target:self action:@selector(changed:)];slider.frame=NSMakeRect(140,375,240,30);slider.accessibilityLabel=@"Volume";[controls addSubview:slider];
    [self label:@"Travelers" frame:NSMakeRect(430,380,100,24) in:controls];NSStepper *stepper=[[NSStepper alloc]initWithFrame:NSMakeRect(550,372,22,32)];stepper.minValue=1;stepper.maxValue=9;stepper.integerValue=2;stepper.accessibilityLabel=@"Travelers";stepper.target=self;stepper.action=@selector(changed:);[controls addSubview:stepper];
    self.volumeValue=[self label:@"4.0" frame:NSMakeRect(385,380,44,24) in:controls];
    self.travelerValue=[self label:@"2" frame:NSMakeRect(590,380,40,24) in:controls];
    NSPopUpButton *popup=[[NSPopUpButton alloc]initWithFrame:NSMakeRect(24,308,230,34) pullsDown:NO];[popup addItemsWithTitles:@[@"Gore",@"Queenstown",@"Dunedin"]];popup.accessibilityLabel=@"Destination";popup.target=self;popup.action=@selector(changed:);[controls addSubview:popup];
    NSComboBox *combo=[[NSComboBox alloc]initWithFrame:NSMakeRect(305,310,240,30)];[combo addItemsWithObjectValues:@[@"Art gallery",@"Fishing",@"Aviation museum"]];combo.stringValue=@"Art gallery";combo.accessibilityLabel=@"Activity";combo.target=self;combo.action=@selector(changed:);[controls addSubview:combo];
    NSSegmentedControl *segments=[NSSegmentedControl segmentedControlWithLabels:@[@"Morning",@"Afternoon",@"Evening"] trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(changed:)];segments.frame=NSMakeRect(24,250,370,32);segments.accessibilityLabel=@"Time of day";segments.selectedSegment=0;[controls addSubview:segments];
    NSDatePicker *date=[[NSDatePicker alloc]initWithFrame:NSMakeRect(24,190,230,30)];date.datePickerStyle=NSDatePickerStyleTextFieldAndStepper;date.datePickerElements=NSDatePickerElementFlagYearMonthDay;date.dateValue=[NSDate dateWithTimeIntervalSince1970:1790208000];date.accessibilityLabel=@"Departure date";date.target=self;date.action=@selector(changed:);[controls addSubview:date];
    NSColorWell *color=[[NSColorWell alloc]initWithFrame:NSMakeRect(310,185,70,34)];color.color=NSColor.systemBlueColor;color.accessibilityLabel=@"Report color";color.target=self;color.action=@selector(changed:);[controls addSubview:color];
    NSProgressIndicator *progress=[[NSProgressIndicator alloc]initWithFrame:NSMakeRect(24,128,300,20)];progress.indeterminate=NO;progress.minValue=0;progress.maxValue=100;progress.doubleValue=65;progress.accessibilityLabel=@"Download progress";[controls addSubview:progress];
    NSLevelIndicator *level=[[NSLevelIndicator alloc]initWithFrame:NSMakeRect(375,128,220,22)];level.minValue=0;level.maxValue=100;level.doubleValue=40;level.accessibilityLabel=@"Disk usage";[controls addSubview:level];
    [self button:@"Open sheet" frame:NSMakeRect(24,60,180,34) in:controls action:@selector(openSheet:)];
    [self button:@"Delayed update" frame:NSMakeRect(220,60,180,34) in:controls action:@selector(delayedUpdate:)];
    NSView *lists=pages[1];
    [self label:@"Select Reservation 072 without manual scrolling" frame:NSMakeRect(20,513,870,24) in:lists];
    NSScrollView *tableScroll=[[NSScrollView alloc]initWithFrame:NSMakeRect(20,55,520,444)];tableScroll.hasVerticalScroller=YES;
    self.table=[[NSTableView alloc]initWithFrame:tableScroll.bounds];self.table.accessibilityLabel=@"Reservations";self.table.rowHeight=27;self.table.allowsMultipleSelection=YES;self.table.dataSource=self;self.table.delegate=self;
    for(NSArray *info in @[@[@"name",@"Reservation",@280],@[@"city",@"City",@140]]){NSTableColumn *column=[[NSTableColumn alloc]initWithIdentifier:info[0]];column.title=info[1];column.width=[info[2] doubleValue];column.editable=NO;[self.table addTableColumn:column];}
    tableScroll.documentView=self.table;[lists addSubview:tableScroll];
    NSScrollView *outlineScroll=[[NSScrollView alloc]initWithFrame:NSMakeRect(565,55,335,444)];outlineScroll.hasVerticalScroller=YES;
    self.outline=[[NSOutlineView alloc]initWithFrame:outlineScroll.bounds];self.outline.accessibilityLabel=@"Trip outline";self.outline.rowHeight=27;
    NSTableColumn *outlineColumn=[[NSTableColumn alloc]initWithIdentifier:@"name"];outlineColumn.title=@"Itinerary";outlineColumn.width=300;outlineColumn.editable=NO;[self.outline addTableColumn:outlineColumn];self.outline.outlineTableColumn=outlineColumn;self.outline.dataSource=self;self.outline.delegate=self;
    NSMutableArray *groups=NSMutableArray.array;for(int day=1;day<=30;day++){NSMutableArray *children=NSMutableArray.array;for(int n=1;n<=3;n++)[children addObject:[NSString stringWithFormat:@"Day %02d activity %d",day,n]];[groups addObject:@{@"name":[NSString stringWithFormat:@"Day %02d",day],@"children":children}];}self.groups=groups;[self.outline reloadData];
    outlineScroll.documentView=self.outline;[lists addSubview:outlineScroll];
    NSView *text=pages[2];[self label:@"Report title" frame:NSMakeRect(24,505,120,24) in:text];
    NSTextField *title=[[NSTextField alloc]initWithFrame:NSMakeRect(150,500,500,30)];title.stringValue=@"Draft trip report";title.accessibilityLabel=@"Report title";title.target=self;title.action=@selector(changed:);[text addSubview:title];
    NSSecureTextField *secret=[[NSSecureTextField alloc]initWithFrame:NSMakeRect(150,450,280,30)];secret.stringValue=@"sample-code";secret.accessibilityLabel=@"Access code";[text addSubview:secret];[self label:@"Access code" frame:NSMakeRect(24,455,120,24) in:text];
    NSScrollView *editorScroll=[[NSScrollView alloc]initWithFrame:NSMakeRect(24,85,875,340)];editorScroll.hasVerticalScroller=YES;editorScroll.borderType=NSBezelBorder;
    NSTextView *editor=[[NSTextView alloc]initWithFrame:editorScroll.bounds];editor.richText=YES;editor.font=[NSFont systemFontOfSize:18];editor.string=@"Plan a quiet morning in Gore. Visit the gallery after lunch.\nThis paragraph is ready for selection and formatting.";editor.accessibilityLabel=@"Trip notes";editor.delegate=self;editor.automaticQuoteSubstitutionEnabled=NO;editor.automaticTextReplacementEnabled=NO;editor.automaticSpellingCorrectionEnabled=NO;editorScroll.documentView=editor;[text addSubview:editorScroll];
    NSButton *bold=[self button:@"Bold selection" frame:NSMakeRect(24,28,180,34) in:text action:@selector(addFontTrait:)];bold.target=[NSFontManager sharedFontManager];bold.tag=NSBoldFontMask;bold.refusesFirstResponder=YES;
    if (self.background) [self.window orderBack:nil];
    else {[self.window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}
}
- (void)countPress:(id)sender {self.status.stringValue=[NSString stringWithFormat:@"Last action: Count press %ld",(long)++self.count];}
- (void)delayedUpdate:(id)sender {
    self.status.stringValue=@"Last action: Waiting for delayed update";
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        self.status.stringValue=@"Last action: Delayed update completed";
        NSAccessibilityPostNotification(self.status, NSAccessibilityValueChangedNotification);
    });
}
- (void)seatChanged:(NSButton *)sender {
    self.windowSeat.state=sender==self.windowSeat?NSControlStateValueOn:NSControlStateValueOff;
    self.aisleSeat.state=sender==self.aisleSeat?NSControlStateValueOn:NSControlStateValueOff;
    [self changed:sender];
}
- (void)changed:(id)sender {
    NSString *name=NSStringFromClass([sender class]);
    id value=@"";
    if ([sender isKindOfClass:NSPopUpButton.class]) {
        NSPopUpButton *control=sender;name=@"Destination";value=control.titleOfSelectedItem ?: @"";
    } else if ([sender isKindOfClass:NSButton.class]) {
        NSButton *control=sender;name=control.title;value=@(control.state);
    } else if ([sender isKindOfClass:NSSwitch.class]) {
        NSSwitch *control=sender;name=@"Notifications";value=@(control.state);
    } else if ([sender isKindOfClass:NSStepper.class]) {
        NSStepper *control=sender;name=@"Travelers";value=@(control.integerValue);self.travelerValue.stringValue=[value stringValue];
    } else if ([sender isKindOfClass:NSSlider.class]) {
        NSSlider *control=sender;name=@"Volume";value=@(control.doubleValue);self.volumeValue.stringValue=[NSString stringWithFormat:@"%.1f",control.doubleValue];
    } else if ([sender isKindOfClass:NSComboBox.class]) {
        NSComboBox *control=sender;name=@"Activity";value=control.stringValue;
    } else if ([sender isKindOfClass:NSDatePicker.class]) {
        NSDatePicker *control=sender;name=@"Departure date";value=control.dateValue;
    } else if ([sender isKindOfClass:NSSegmentedControl.class]) {
        NSSegmentedControl *control=sender;name=@"Time of day";value=control.selectedSegment>=0 ? [control labelForSegment:control.selectedSegment] : @"None";
    } else if ([sender isKindOfClass:NSColorWell.class]) {
        NSColorWell *control=sender;name=@"Report color";value=control.color;
    } else if ([sender isKindOfClass:NSTextField.class]) {
        NSTextField *control=sender;name=@"Report title";value=control.stringValue;
    }
    self.status.stringValue=[NSString stringWithFormat:@"Last action: %@ = %@",name,value];
}
- (void)openSheet:(id)sender {NSAlert *alert=[NSAlert new];alert.messageText=@"Trip confirmation";alert.informativeText=@"A native sheet for accessibility testing.";[alert addButtonWithTitle:@"Confirm trip"];[alert addButtonWithTitle:@"Cancel"];[alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response){self.status.stringValue=response==NSAlertFirstButtonReturn?@"Last action: Trip confirmed":@"Last action: Trip cancelled";}];}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {return 120;}
- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {return [column.identifier isEqual:@"name"]?[NSString stringWithFormat:@"Reservation %03ld",(long)row+1]:@[@"Gore",@"Queenstown",@"Dunedin"][row%3];}
- (void)tableViewSelectionDidChange:(NSNotification *)note {self.status.stringValue=self.table.selectedRow<0?@"Last action: No reservation selected":[NSString stringWithFormat:@"Last action: Reservation %03ld selected (%lu total)",(long)self.table.selectedRow+1,(unsigned long)self.table.selectedRowIndexes.count];}
- (NSInteger)outlineView:(NSOutlineView *)outlineView numberOfChildrenOfItem:(id)item {return item?[item isKindOfClass:NSDictionary.class]?[item[@"children"] count]:0:self.groups.count;}
- (id)outlineView:(NSOutlineView *)outlineView child:(NSInteger)index ofItem:(id)item {return item?item[@"children"][index]:self.groups[index];}
- (BOOL)outlineView:(NSOutlineView *)outlineView isItemExpandable:(id)item {return [item isKindOfClass:NSDictionary.class];}
- (id)outlineView:(NSOutlineView *)outlineView objectValueForTableColumn:(NSTableColumn *)column byItem:(id)item {return [item isKindOfClass:NSDictionary.class]?item[@"name"]:item;}
- (void)outlineViewSelectionDidChange:(NSNotification *)note {id item=self.outline.selectedRow<0?nil:[self.outline itemAtRow:self.outline.selectedRow];self.status.stringValue=item?[NSString stringWithFormat:@"Last action: %@ selected",[item isKindOfClass:NSDictionary.class]?item[@"name"]:item]:@"Last action: No itinerary item selected";}
- (void)textDidChange:(NSNotification *)note {self.status.stringValue=@"Last action: Trip notes edited";}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {return YES;}
@end

int main(int argc,const char **argv){@autoreleasepool{[NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];LabDelegate *delegate=[LabDelegate new];for(int index=1;index<argc;index++)if(strcmp(argv[index],"--background")==0)delegate.background=YES;NSApp.delegate=delegate;[NSApp run];}return 0;}
