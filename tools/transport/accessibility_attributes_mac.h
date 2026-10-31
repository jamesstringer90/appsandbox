#ifndef ASB_ACCESSIBILITY_ATTRIBUTES_MAC_H
#define ASB_ACCESSIBILITY_ATTRIBUTES_MAC_H

#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

static inline BOOL AsbAXAttributeRequiresLiveQuery(NSString *attribute) {
    static NSSet<NSString *> *snapshotAttributes;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        snapshotAttributes = [NSSet setWithArray:@[
            @"AXRole", @"AXSubrole", @"AXRoleDescription", @"AXIdentifier",
            @"AXTitle", @"AXDescription", @"AXAttributedTitle", @"AXAttributedDescription",
            @"AXHelp", @"AXValue", @"AXValueDescription", @"AXPlaceholderValue",
            @"AXEnabled", @"AXFocused", @"AXSelected", @"AXExpanded", @"AXDisclosing",
            @"AXRequired", @"AXHidden", @"AXElementBusy", @"AXMain", @"AXMinimized", @"AXModal",
            @"AXPosition", @"AXSize", @"AXOrientation", @"AXMinValue", @"AXMaxValue",
            @"AXValueIncrement", @"AXValueWraps", @"AXURL", @"AXDocument", @"AXFilename",
            @"AXParent", @"AXChildren", @"AXVisibleChildren", @"AXSelectedChildren",
            @"AXWindow", @"AXTopLevelUIElement", @"AXFocusedUIElement",
            @"AXTitleUIElement", @"AXServesAsTitleForUIElements", @"AXLinkedUIElements",
            @"AXContents", @"AXTabs", @"AXHeader", @"AXSplitters", @"AXHandles",
            @"AXCloseButton", @"AXMinimizeButton", @"AXZoomButton", @"AXFullScreenButton",
            @"AXDefaultButton", @"AXCancelButton", @"AXToolbarButton", @"AXOverflowButton",
            @"AXHorizontalScrollBar", @"AXVerticalScrollBar", @"AXIncrementor",
            @"AXIncrementButton", @"AXDecrementButton",
            @"AXRows", @"AXVisibleRows", @"AXSelectedRows", @"AXRowCount",
            @"AXColumns", @"AXVisibleColumns", @"AXSelectedColumns", @"AXColumnCount",
            @"AXVisibleCells", @"AXColumnTitles", @"AXRowHeaderUIElements", @"AXColumnHeaderUIElements",
            @"AXRowIndexRange", @"AXColumnIndexRange", @"AXDisclosedRows", @"AXDisclosedByRow", @"AXDisclosureLevel",
            @"AXSelectedText", @"AXSelectedTextRange", @"AXSelectedTextRanges",
            @"AXNumberOfCharacters", @"AXInsertionPointLineNumber",
            @"AXMenuItemCmdChar", @"AXMenuItemCmdVirtualKey", @"AXMenuItemCmdGlyph",
            @"AXMenuItemCmdModifiers", @"AXMenuItemMarkChar", @"AXMenuItemPrimaryUIElement"
        ]];
    });
    return ![snapshotAttributes containsObject:attribute];
}

static inline NSArray<NSString *> *AsbAXCaptureAttributeNames(NSArray<NSString *> *names) {
    NSMutableArray *captured = [NSMutableArray arrayWithCapacity:names.count];
    for (NSString *name in names)
        if (!AsbAXAttributeRequiresLiveQuery(name)) [captured addObject:name];
    return captured;
}

#endif
