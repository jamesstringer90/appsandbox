#import "vm_accessibility_uia.h"
#import "../../tools/transport/accessibility_value_mac.h"

static NSDictionary *AXValue(NSString *type, id value) {
    return @{@"$ax": type, @"value": value};
}

static id AXData(id value, NSString *type) {
    return [value isKindOfClass:NSDictionary.class] && [value[@"$ax"] isEqual:type] ? value[@"value"] : nil;
}

static NSString *UiaReference(id value) {
    if (![value isKindOfClass:NSDictionary.class] || ![value[@"$uia"] isEqual:@"element"] ||
        !VmAccessibilityIsIdentifier(value[@"value"])) return nil;
    return value[@"value"];
}

static NSArray *AXReferences(NSArray *values) {
    NSMutableArray *result = NSMutableArray.array;
    for (id value in values) [result addObject:AXValue(@"element", UiaReference(value))];
    return result;
}

static BOOL UiaRecordValid(id record) {
    if (![record isKindOfClass:NSDictionary.class] || !VmAccessibilityIsIdentifier(record[@"id"]) ||
        (record[@"parent"] && !VmAccessibilityIsIdentifier(record[@"parent"])) ||
        ![record[@"children"] isKindOfClass:NSArray.class] || [record[@"children"] count] > ASB_AX_MAX_NODES) return NO;
    for (id child in record[@"children"]) if (!VmAccessibilityIsIdentifier(child)) return NO;
    NSDictionary *p = record[@"properties"];
    if (![p isKindOfClass:NSDictionary.class] || p.count > 128 ||
        !VmAccessibilityIsInteger(p[@"ControlType"], 0, INT_MAX) || !AsbAXFiniteArray(p[@"BoundingRectangle"], 4, NO)) return NO;
    if (p[@"Text.DocumentText"] && !VmAccessibilityIsString(p[@"Text.DocumentText"], ASB_AX_MAX_STRING)) return NO;
    for (id key in p) {
        if (!VmAccessibilityIsString(key, 128)) return NO;
        id value = p[key];
        if ([@[@"Name", @"AutomationId", @"HelpText", @"LocalizedControlType", @"ValueValue", @"Text.DocumentText"] containsObject:key]) {
            if (!VmAccessibilityIsString(value, ASB_AX_MAX_STRING)) return NO;
        } else if ([key isEqual:@"BoundingRectangle"]) {
            for (NSUInteger i = 0; i < 4; i++) if (!VmAccessibilityIsNumber(value[i], i < 2 ? -100000000 : 0, 100000000)) return NO;
        } else if ([key isEqual:@"SelectionItemSelectionContainer"]) {
            if (!UiaReference(value)) return NO;
        } else if ([@[@"SelectionSelection", @"TableColumnHeaders", @"TableRowHeaders", @"TableItemColumnHeaderItems", @"TableItemRowHeaderItems"] containsObject:key]) {
            if (![value isKindOfClass:NSArray.class] || [value count] > ASB_AX_MAX_NODES) return NO;
            for (id reference in value) if (!UiaReference(reference)) return NO;
        } else if ([key isEqual:@"Text.Selection"]) {
            if (![value isKindOfClass:NSArray.class] || [value count] > ASB_AX_MAX_NODES) return NO;
            for (id range in value) if (!AsbAXFiniteArray(range, 2, YES) ||
                [range[0] doubleValue] + [range[1] doubleValue] > [p[@"Text.DocumentText"] length]) return NO;
        } else if (!VmAccessibilityIsNumber(value, -DBL_MAX, DBL_MAX)) return NO;
    }
    return YES;
}

static NSString *AXRole(NSDictionary *p) {
    switch ([p[@"ControlType"] integerValue]) {
        case 50000: return @"AXButton";
        case 50002: return @"AXCheckBox";
        case 50003: return [p[@"IsValuePatternAvailable"] boolValue] ? @"AXComboBox" : @"AXPopUpButton";
        case 50004: return @"AXTextField";
        case 50005: return @"AXLink";
        case 50006: return @"AXImage";
        case 50007: case 50024: return @"AXRow";
        case 50008: return @"AXList";
        case 50009: return @"AXMenu";
        case 50010: return @"AXMenuBar";
        case 50011: return @"AXMenuItem";
        case 50012: return @"AXProgressIndicator";
        case 50013: case 50019: return @"AXRadioButton";
        case 50014: return @"AXScrollBar";
        case 50015: return @"AXSlider";
        case 50016: return @"AXIncrementor";
        case 50018: return @"AXTabGroup";
        case 50020: return @"AXStaticText";
        case 50021: return @"AXToolbar";
        case 50022: return @"AXHelpTag";
        case 50023: return @"AXOutline";
        case 50025: return [p[@"IsGridItemPatternAvailable"] boolValue] ? @"AXCell" : @"AXGroup";
        case 50028: case 50036: return @"AXTable";
        case 50029: return [p[@"IsGridItemPatternAvailable"] boolValue] ? @"AXCell" : @"AXRow";
        case 50030: return @"AXTextArea";
        case 50032: return @"AXWindow";
        case 50035: return @"AXColumn";
        case 50038: return @"AXSplitter";
        default: return @"AXGroup";
    }
}

static void AXCollectStructure(NSDictionary *record, NSDictionary *records, NSUInteger depth, BOOL collecting,
    NSMutableArray *rows, NSMutableArray *columns) {
    for (NSString *identifier in record[@"children"]) {
        NSDictionary *child = records[identifier];
        if (!child) continue;
        NSString *role = AXRole(child[@"properties"]);
        if ([role isEqual:@"AXRow"]) [rows addObject:AXValue(@"element", identifier)];
        else if ([role isEqual:@"AXColumn"]) [columns addObject:AXValue(@"element", identifier)];
        else if (collecting && depth < 2 && [role isEqual:@"AXGroup"])
            AXCollectStructure(child, records, depth + 1, collecting, rows, columns);
    }
}

static NSDictionary *AXRecord(NSDictionary *record, NSDictionary *records) {
    NSDictionary *p = record[@"properties"];
    BOOL enabled = [p[@"IsEnabled"] boolValue], secure = !p[@"IsPassword"] || [p[@"IsPassword"] boolValue];
    BOOL value = [p[@"IsValuePatternAvailable"] boolValue], range = [p[@"IsRangeValuePatternAvailable"] boolValue];
    BOOL toggle = [p[@"IsTogglePatternAvailable"] boolValue], selection = [p[@"IsSelectionItemPatternAvailable"] boolValue];
    BOOL invoke = [p[@"IsInvokePatternAvailable"] boolValue];
    NSString *role = AXRole(p);
    NSMutableDictionary *node = [@{@"id": record[@"id"], @"role": role, @"label": p[@"Name"] ?: @"",
        @"children": record[@"children"], @"frame": p[@"BoundingRectangle"], @"enabled": @(enabled),
        @"focused": @([p[@"HasKeyboardFocus"] boolValue]), @"secure": @(secure)} mutableCopy];
    if (record[@"parent"]) node[@"parent"] = record[@"parent"];
    if (secure) node[@"subrole"] = @"AXSecureTextField";
    NSMutableDictionary *attributes = NSMutableDictionary.dictionary;
    NSMutableOrderedSet *writable = NSMutableOrderedSet.orderedSet, *actions = NSMutableOrderedSet.orderedSet;
    NSMutableArray *parameters = NSMutableArray.array;
    NSDictionary *strings = @{@"AutomationId": @"AXIdentifier", @"HelpText": @"AXHelp", @"LocalizedControlType": @"AXRoleDescription"};
    for (NSString *key in strings) if ([p[key] length]) attributes[strings[key]] = p[key];
    if ([p[@"Orientation"] integerValue]) attributes[@"AXOrientation"] = [p[@"Orientation"] integerValue] == 1 ? @"AXHorizontalOrientation" : @"AXVerticalOrientation";
    attributes[@"AXVisible"] = @(![p[@"IsOffscreen"] boolValue]);
    if (enabled && [p[@"IsKeyboardFocusable"] boolValue]) [writable addObject:@"AXFocused"];
    if (value) {
        if (!secure || AsbAXIsMaskedValue(p[@"ValueValue"])) attributes[@"AXValue"] = p[@"ValueValue"] ?: @"";
        if (enabled && p[@"ValueIsReadOnly"] && ![p[@"ValueIsReadOnly"] boolValue]) [writable addObject:@"AXValue"];
    }
    if (range && !secure) {
        NSDictionary *numbers = @{@"RangeValueValue": @"AXValue", @"RangeValueMinimum": @"AXMinValue", @"RangeValueMaximum": @"AXMaxValue"};
        for (NSString *key in numbers) if (p[key]) attributes[numbers[key]] = AXValue(@"double", p[key]);
        if (enabled && p[@"RangeValueIsReadOnly"] && ![p[@"RangeValueIsReadOnly"] boolValue]) {
            [writable addObject:@"AXValue"];
            if ([p[@"RangeValueSmallChange"] doubleValue] > 0) [actions addObjectsFromArray:@[@"AXIncrement", @"AXDecrement"]];
        }
    }
    if (toggle && !secure) {
        attributes[@"AXValue"] = p[@"ToggleToggleState"] ?: @0;
        if (enabled && !value && !range) [writable addObject:@"AXValue"];
    }
    if (selection) {
        attributes[@"AXSelected"] = @([p[@"SelectionItemIsSelected"] boolValue]);
        if ([role isEqual:@"AXRadioButton"]) attributes[@"AXValue"] = @([p[@"SelectionItemIsSelected"] boolValue] ? 1 : 0);
        if (enabled) [writable addObject:@"AXSelected"];
    }
    if (enabled && (invoke || toggle || selection)) [actions addObject:@"AXPress"];
    if ([p[@"IsExpandCollapsePatternAvailable"] boolValue] && p[@"ExpandCollapseExpandCollapseState"] &&
        [p[@"ExpandCollapseExpandCollapseState"] integerValue] != 3) {
        NSInteger state = [p[@"ExpandCollapseExpandCollapseState"] integerValue];
        BOOL expanded = state == 1 || state == 2;
        attributes[@"AXExpanded"] = attributes[@"AXDisclosing"] = @(expanded);
        if (enabled) {
            [writable addObjectsFromArray:@[@"AXExpanded", @"AXDisclosing"]];
            [actions addObject:@"AXShowMenu"];
            if (expanded) [actions addObject:@"AXCancel"];
            if (!invoke && !toggle && !selection) [actions addObject:@"AXPress"];
        }
    }
    if (enabled && ([p[@"IsScrollItemPatternAvailable"] boolValue] || [p[@"IsVirtualizedItemPatternAvailable"] boolValue]))
        [actions addObject:@"AXScrollToVisible"];
    if (enabled && [p[@"IsScrollPatternAvailable"] boolValue]) {
        if ([p[@"ScrollVerticallyScrollable"] boolValue]) [actions addObjectsFromArray:@[@"AXScrollDownByPage", @"AXScrollUpByPage"]];
        if ([p[@"ScrollHorizontallyScrollable"] boolValue]) [actions addObjectsFromArray:@[@"AXScrollRightByPage", @"AXScrollLeftByPage"]];
    }
    if ([p[@"IsSelectionPatternAvailable"] boolValue]) {
        attributes[@"AXAllowsMultipleSelection"] = @([p[@"SelectionCanSelectMultiple"] boolValue]);
        attributes[@"AXSelectionRequired"] = @([p[@"SelectionIsSelectionRequired"] boolValue]);
        if (enabled) [writable addObjectsFromArray:@[@"AXSelectedChildren", @"AXSelectedRows"]];
    }
    if ([p[@"IsGridPatternAvailable"] boolValue]) {
        attributes[@"AXRowCount"] = p[@"GridRowCount"] ?: @0;
        attributes[@"AXColumnCount"] = p[@"GridColumnCount"] ?: @0;
        [parameters addObject:@"AXCellForColumnAndRow"];
    }
    if ([p[@"IsGridItemPatternAvailable"] boolValue]) {
        if (p[@"GridItemRow"] && [p[@"GridItemRow"] integerValue] >= 0)
            attributes[@"AXRowIndexRange"] = AXValue(@"range", @[p[@"GridItemRow"], @(MAX(1, [p[@"GridItemRowSpan"] integerValue]))]);
        if (p[@"GridItemColumn"] && [p[@"GridItemColumn"] integerValue] >= 0)
            attributes[@"AXColumnIndexRange"] = AXValue(@"range", @[p[@"GridItemColumn"], @(MAX(1, [p[@"GridItemColumnSpan"] integerValue]))]);
    }
    if ([p[@"IsWindowPatternAvailable"] boolValue]) attributes[@"AXModal"] = @([p[@"WindowIsModal"] boolValue]);
    NSInteger control = [p[@"ControlType"] integerValue];
    NSString *text = p[@"Text.DocumentText"];
    if (!secure && text && (control == 50004 || control == 50030)) {
        attributes[@"AXValue"] = text;
        attributes[@"AXNumberOfCharacters"] = @(text.length);
        [parameters addObjectsFromArray:@[@"AXStringForRange", @"AXBoundsForRange", @"AXAttributedStringForRange", @"AXRangeForPosition"]];
        NSArray *selected = p[@"Text.Selection"];
        if (selected.count == 1) {
            NSArray *bounds = selected[0];
            attributes[@"AXSelectedTextRange"] = AXValue(@"range", bounds);
            attributes[@"AXSelectedText"] = [text substringWithRange:NSMakeRange([bounds[0] unsignedIntegerValue], [bounds[1] unsignedIntegerValue])];
            if (enabled) [writable addObject:@"AXSelectedTextRange"];
        }
    }
    NSDictionary *relations = @{@"SelectionSelection": @"AXSelectedChildren", @"TableColumnHeaders": @"AXColumnHeaderUIElements",
        @"TableRowHeaders": @"AXRowHeaderUIElements", @"TableItemColumnHeaderItems": @"AXColumnHeaderUIElements", @"TableItemRowHeaderItems": @"AXRowHeaderUIElements"};
    for (NSString *key in @[@"SelectionSelection", @"TableColumnHeaders", @"TableRowHeaders", @"TableItemColumnHeaderItems", @"TableItemRowHeaderItems"])
        if (p[key]) attributes[relations[key]] = AXReferences(p[key]);
    if (attributes[@"AXSelectedChildren"]) attributes[@"AXSelectedRows"] = attributes[@"AXSelectedChildren"];
    NSMutableArray *visible = NSMutableArray.array, *contents = NSMutableArray.array;
    for (NSString *identifier in record[@"children"]) if (records[identifier]) {
        NSDictionary *reference = AXValue(@"element", identifier);
        [contents addObject:reference];
        if (![records[identifier][@"properties"][@"IsOffscreen"] boolValue]) [visible addObject:reference];
    }
    attributes[@"AXVisibleChildren"] = visible;
    if ([@[@"AXTable", @"AXOutline", @"AXList"] containsObject:role]) {
        NSMutableArray *rows = NSMutableArray.array, *columns = NSMutableArray.array;
        AXCollectStructure(record, records, 0, YES, rows, columns);
        attributes[@"AXRows"] = rows; attributes[@"AXContents"] = contents;
        if (columns.count) attributes[@"AXColumns"] = columns;
    }
    NSMutableOrderedSet *names = [NSMutableOrderedSet orderedSetWithArray:@[@"AXRole", @"AXRoleDescription", @"AXParent", @"AXWindow", @"AXTopLevelUIElement",
        @"AXChildren", @"AXPosition", @"AXSize", @"AXEnabled", @"AXFocused", @"AXDescription"]];
    if (secure) [names addObject:@"AXSubrole"];
    [names addObjectsFromArray:[attributes.allKeys sortedArrayUsingSelector:@selector(compare:)]];
    node[@"attributes"] = attributes; node[@"attributeNames"] = names.array;
    node[@"writableAttributes"] = writable.array; node[@"actions"] = actions.array; node[@"parameterizedNames"] = parameters;
    return VmAccessibilityParseRecord(node);
}

static NSDictionary *AXFailure(AXError error) { return @{@"ok": @NO, @"error": @(error)}; }
static NSDictionary *AXSuccess(id value) { return value ? @{@"ok": @YES, @"value": value} : @{@"ok": @YES}; }

static NSDictionary *AXResponse(NSDictionary *response, NSString *action) {
    if (!response) return AXFailure(kAXErrorCannotComplete);
    if ([response[@"ok"] boolValue]) return response;
    if (!VmAccessibilityIsInteger(response[@"error"], INT32_MIN, UINT32_MAX)) return AXFailure(kAXErrorCannotComplete);
    uint32_t error = [response[@"error"] unsignedIntValue];
    if (error == 0x80040201) return AXFailure(kAXErrorInvalidUIElement);
    if (error == 0x80070057) return AXFailure(kAXErrorIllegalArgument);
    if (error == 0x80040204) return AXFailure([action isEqual:@"getParameterizedAttribute"] ? kAXErrorParameterizedAttributeUnsupported :
        [action hasSuffix:@"Attribute"] || [action isEqual:@"isAttributeSettable"] ? kAXErrorAttributeUnsupported : kAXErrorActionUnsupported);
    return AXFailure(kAXErrorCannotComplete);
}

@implementation VmAccessibilityUia {
    NSLock *_lock;
    NSLock *_requestLock;
    NSDictionary *_records;
    NSString *_session;
}

- (instancetype)init {
    self = [super init];
    if (self) { _lock = NSLock.new; _requestLock = NSLock.new; }
    return self;
}

- (VmAccessibilitySnapshot *)snapshot:(NSDictionary *)message {
    if (![message[@"nodes"] isKindOfClass:NSArray.class] || [message[@"nodes"] count] > ASB_AX_MAX_NODES ||
        ![message[@"app"] isKindOfClass:NSDictionary.class] || !VmAccessibilityIsInteger(message[@"app"][@"pid"], 1, INT_MAX)) return nil;
    NSMutableDictionary *records = NSMutableDictionary.dictionary;
    for (id record in message[@"nodes"]) {
        if (!UiaRecordValid(record) || records[record[@"id"]]) return nil;
        records[record[@"id"]] = record;
    }
    NSMutableArray *nodes = NSMutableArray.array;
    for (NSDictionary *record in message[@"nodes"]) {
        NSDictionary *node = AXRecord(record, records);
        if (!node) return nil;
        [nodes addObject:node];
    }
    NSMutableDictionary *translated = [message mutableCopy], *app = [message[@"app"] mutableCopy];
    app[@"bundleId"] = [NSString stringWithFormat:@"win32.%@", app[@"pid"]];
    translated[@"app"] = app; translated[@"nodes"] = nodes;
    VmAccessibilitySnapshot *snapshot = VmAccessibilityParseSnapshot(translated);
    if (snapshot) {
        [_lock lock]; _records = [records copy]; _session = snapshot.session; [_lock unlock];
    }
    return snapshot;
}

- (NSDictionary *)request:(NSDictionary *)message send:(NSDictionary *(^)(NSDictionary *))send {
    NSTimeInterval remaining = [message[@"hostDeadline"] doubleValue] - VmAccessibilityNow();
    if (remaining <= 0 || ![_requestLock lockBeforeDate:[NSDate dateWithTimeIntervalSinceNow:remaining]]) return AXFailure(kAXErrorCannotComplete);
    @try { return [self translateRequest:message send:send]; }
    @finally { [_requestLock unlock]; }
}

- (NSDictionary *)translateRequest:(NSDictionary *)message send:(NSDictionary *(^)(NSDictionary *))send {
    [_lock lock];
    BOOL current = [_session isEqual:message[@"session"]];
    NSDictionary *records = _records;
    [_lock unlock];
    if (!current) return AXFailure(kAXErrorInvalidUIElement);
    NSString *action = message[@"action"], *attribute = message[@"attribute"], *nodeID = message[@"nodeId"];
    id value = message[@"value"];
    NSDictionary *(^call)(NSString *, NSString *, id, NSString *) = ^NSDictionary *(NSString *operation, NSString *identifier, id argument, NSString *property) {
        NSMutableDictionary *request = [message mutableCopy];
        request[@"action"] = operation; request[@"nodeId"] = identifier;
        [request removeObjectForKey:@"value"]; [request removeObjectForKey:@"attribute"];
        if (argument) request[@"value"] = argument;
        if (property) request[@"attribute"] = property;
        return AXResponse(send(request), action);
    };
    if ([action isEqual:@"getFrame"]) return call(@"GetBoundingRectangle", nodeID, nil, nil);
    if ([action isEqual:@"hitTest"]) {
        id point = AXData(value, @"point");
        if (!AsbAXFiniteArray(point, 2, NO)) return AXFailure(kAXErrorIllegalArgument);
        NSDictionary *response = call(@"ElementFromPoint", nodeID, point, nil);
        if (![response[@"ok"] boolValue]) return response;
        NSString *identifier = UiaReference(response[@"value"]);
        return identifier ? AXSuccess(AXValue(@"element", identifier)) : AXFailure(kAXErrorCannotComplete);
    }
    if ([action isEqual:@"getParameterizedAttribute"]) return [self parameter:attribute value:value node:nodeID call:call];
    NSDictionary *response = nil, *record = records[nodeID];
    BOOL fresh = [@[@"getMetadata", @"getAttribute", @"isAttributeSettable", @"AXIncrement", @"AXDecrement"] containsObject:action] ||
        ([action isEqual:@"setAttribute"] && [@[@"AXSelected", @"AXSelectedChildren", @"AXSelectedRows"] containsObject:attribute]);
    if (!record || fresh) {
        response = call(@"GetMetadata", nodeID, nil, nil);
        if (![response[@"ok"] boolValue]) return response;
        record = response[@"record"];
    }
    if (!UiaRecordValid(record) || ![record[@"id"] isEqual:nodeID]) return AXFailure(kAXErrorInvalidUIElement);
    NSMutableDictionary *available = [records mutableCopy]; available[nodeID] = record;
    NSDictionary *node = AXRecord(record, available), *p = record[@"properties"];
    if (!node) return AXFailure(kAXErrorCannotComplete);
    if ([action isEqual:@"getMetadata"]) return @{@"ok": @YES, @"record": node};
    if ([action isEqual:@"getAttribute"]) return node[@"attributes"][attribute] ? AXSuccess(node[@"attributes"][attribute]) : AXFailure(kAXErrorAttributeUnsupported);
    if ([action isEqual:@"isAttributeSettable"]) return AXSuccess(@([node[@"writableAttributes"] containsObject:attribute]));
    if ([action isEqual:@"setAttribute"]) {
        if (![node[@"writableAttributes"] containsObject:attribute]) return AXFailure(kAXErrorAttributeUnsupported);
        if ([attribute isEqual:@"AXSelectedChildren"] || [attribute isEqual:@"AXSelectedRows"])
            return [self selection:value container:record call:call];
        if ([attribute isEqual:@"AXSelectedTextRange"]) {
            id range = AXData(value, @"range");
            return AsbAXFiniteArray(range, 2, YES) ? call(@"Text.Select", nodeID, range, nil) : AXFailure(kAXErrorIllegalArgument);
        }
        id number = AXData(value, @"double") ?: value;
        if ([@[@"AXFocused", @"AXSelected", @"AXExpanded", @"AXDisclosing"] containsObject:attribute]) {
            if (!VmAccessibilityIsInteger(number, 0, 1)) return AXFailure(kAXErrorIllegalArgument);
            BOOL flag = [number boolValue];
            if ([attribute isEqual:@"AXFocused"]) return flag ? call(@"SetFocus", nodeID, nil, nil) : AXFailure(kAXErrorIllegalArgument);
            if ([attribute isEqual:@"AXSelected"]) {
                BOOL multiple = NO;
                NSString *container = UiaReference(p[@"SelectionItemSelectionContainer"]);
                if (flag && container) {
                    response = call(@"GetProperty", container, nil, @"SelectionCanSelectMultiple");
                    if (![response[@"ok"] boolValue]) return response;
                    if (!VmAccessibilityIsInteger(response[@"value"], 0, 1)) return AXFailure(kAXErrorCannotComplete);
                    multiple = [response[@"value"] boolValue];
                }
                return call(!flag ? @"SelectionItem.RemoveFromSelection" : multiple ? @"SelectionItem.AddToSelection" : @"SelectionItem.Select", nodeID, nil, nil);
            }
            return call(flag ? @"ExpandCollapse.Expand" : @"ExpandCollapse.Collapse", nodeID, nil, nil);
        }
        if ([attribute isEqual:@"AXValue"]) {
            if ([p[@"IsRangeValuePatternAvailable"] boolValue]) return VmAccessibilityIsNumber(number, -DBL_MAX, DBL_MAX) ?
                call(@"RangeValue.SetValue", nodeID, number, nil) : AXFailure(kAXErrorIllegalArgument);
            if ([p[@"IsValuePatternAvailable"] boolValue]) return VmAccessibilityIsString(value, ASB_AX_MAX_STRING) ?
                call(@"Value.SetValue", nodeID, value, nil) : AXFailure(kAXErrorIllegalArgument);
            if (!VmAccessibilityIsInteger(number, 0, 2)) return AXFailure(kAXErrorIllegalArgument);
            for (NSUInteger i = 0; i < 3; i++) {
                response = call(@"GetProperty", nodeID, nil, @"ToggleToggleState");
                if (![response[@"ok"] boolValue]) return response;
                if (!VmAccessibilityIsInteger(response[@"value"], 0, 2)) return AXFailure(kAXErrorCannotComplete);
                if ([response[@"value"] isEqual:number]) return AXSuccess(nil);
                if (i == 2) return AXFailure(kAXErrorAttributeUnsupported);
                response = call(@"Toggle.Toggle", nodeID, nil, nil);
                if (![response[@"ok"] boolValue]) return response;
            }
        }
        return AXFailure(kAXErrorAttributeUnsupported);
    }
    if (![node[@"actions"] containsObject:action]) return AXFailure(kAXErrorActionUnsupported);
    if ([action isEqual:@"AXPress"]) {
        if (![p[@"IsInvokePatternAvailable"] boolValue] && ![p[@"IsTogglePatternAvailable"] boolValue] &&
            ![p[@"IsSelectionItemPatternAvailable"] boolValue]) {
            response = call(@"GetProperty", nodeID, nil, @"ExpandCollapseExpandCollapseState");
            if (![response[@"ok"] boolValue]) return response;
            if (!VmAccessibilityIsInteger(response[@"value"], 0, 3)) return AXFailure(kAXErrorCannotComplete);
            return call([response[@"value"] integerValue] == 1 ? @"ExpandCollapse.Collapse" : @"ExpandCollapse.Expand", nodeID, nil, nil);
        }
        NSString *operation = [p[@"IsInvokePatternAvailable"] boolValue] ? @"Invoke.Invoke" :
            [p[@"IsTogglePatternAvailable"] boolValue] ? @"Toggle.Toggle" : @"SelectionItem.Select";
        return call(operation, nodeID, nil, nil);
    }
    if ([action isEqual:@"AXIncrement"] || [action isEqual:@"AXDecrement"]) {
        for (NSString *key in @[@"RangeValueValue", @"RangeValueSmallChange", @"RangeValueMinimum", @"RangeValueMaximum"])
            if (!VmAccessibilityIsNumber(p[key], -DBL_MAX, DBL_MAX)) return AXFailure(kAXErrorCannotComplete);
        double minimum = [p[@"RangeValueMinimum"] doubleValue], maximum = [p[@"RangeValueMaximum"] doubleValue];
        double change = [p[@"RangeValueSmallChange"] doubleValue], currentValue = [p[@"RangeValueValue"] doubleValue];
        if (minimum > maximum || change <= 0) return AXFailure(kAXErrorActionUnsupported);
        double target = fmax(minimum, fmin(maximum, currentValue + ([action isEqual:@"AXIncrement"] ? change : -change)));
        return call(@"RangeValue.SetValue", nodeID, @(target), nil);
    }
    if ([action isEqual:@"AXScrollToVisible"]) {
        if ([p[@"IsVirtualizedItemPatternAvailable"] boolValue]) {
            response = call(@"VirtualizedItem.Realize", nodeID, nil, nil);
            if (![response[@"ok"] boolValue]) return response;
        }
        return call(@"ScrollItem.ScrollIntoView", nodeID, nil, nil);
    }
    if ([action isEqual:@"AXShowMenu"] || [action isEqual:@"AXCancel"])
        return call([action isEqual:@"AXShowMenu"] ? @"ExpandCollapse.Expand" : @"ExpandCollapse.Collapse", nodeID, nil, nil);
    NSDictionary *scrolls = @{@"AXScrollDownByPage": @[@2, @3], @"AXScrollUpByPage": @[@2, @0],
        @"AXScrollRightByPage": @[@3, @2], @"AXScrollLeftByPage": @[@0, @2]};
    return scrolls[action] ? call(@"Scroll.Scroll", nodeID, scrolls[action], nil) : AXFailure(kAXErrorActionUnsupported);
}

- (NSDictionary *)selection:(id)value container:(NSDictionary *)record
    call:(NSDictionary *(^)(NSString *, NSString *, id, NSString *))call {
    NSDictionary *p = record[@"properties"];
    if (![value isKindOfClass:NSArray.class] || [value count] > ASB_AX_MAX_NODES ||
        (![p[@"SelectionCanSelectMultiple"] boolValue] && [value count] > 1) ||
        ((!p[@"SelectionIsSelectionRequired"] || [p[@"SelectionIsSelectionRequired"] boolValue]) && ![value count])) return AXFailure(kAXErrorIllegalArgument);
    NSMutableOrderedSet *identifiers = NSMutableOrderedSet.orderedSet;
    for (id reference in value) {
        NSString *identifier = AXData(reference, @"element");
        if (!VmAccessibilityIsIdentifier(identifier) || [identifiers containsObject:identifier]) return AXFailure(kAXErrorIllegalArgument);
        NSDictionary *response = call(@"GetMetadata", identifier, nil, nil), *item = response[@"record"];
        if (![response[@"ok"] boolValue]) return response;
        if (!UiaRecordValid(item) || ![item[@"id"] isEqual:identifier] ||
            ![UiaReference(item[@"properties"][@"SelectionItemSelectionContainer"]) isEqual:record[@"id"]]) return AXFailure(kAXErrorIllegalArgument);
        if (![item[@"properties"][@"IsEnabled"] boolValue]) return AXFailure(kAXErrorCannotComplete);
        [identifiers addObject:identifier];
    }
    if (!identifiers.count) {
        if (!p[@"SelectionSelection"]) return AXFailure(kAXErrorCannotComplete);
        for (id reference in p[@"SelectionSelection"]) {
            NSDictionary *response = call(@"SelectionItem.RemoveFromSelection", UiaReference(reference), nil, nil);
            if (![response[@"ok"] boolValue]) return response;
        }
    } else for (NSUInteger i = 0; i < identifiers.count; i++) {
        NSDictionary *response = call(i ? @"SelectionItem.AddToSelection" : @"SelectionItem.Select", identifiers[i], nil, nil);
        if (![response[@"ok"] boolValue]) return response;
    }
    return AXSuccess(nil);
}

- (NSDictionary *)parameter:(NSString *)attribute value:(id)value node:(NSString *)nodeID
    call:(NSDictionary *(^)(NSString *, NSString *, id, NSString *))call {
    NSDictionary *operations = @{@"AXStringForRange": @"Text.GetText", @"AXBoundsForRange": @"Text.GetBoundingRectangles",
        @"AXAttributedStringForRange": @"Text.GetAttributeRuns", @"AXRangeForPosition": @"Text.RangeFromPoint", @"AXCellForColumnAndRow": @"Grid.GetItem"};
    NSString *operation = operations[attribute];
    if (!operation) return AXFailure(kAXErrorParameterizedAttributeUnsupported);
    BOOL point = [attribute isEqual:@"AXRangeForPosition"], cell = [attribute isEqual:@"AXCellForColumnAndRow"];
    id argument = cell ? value : AXData(value, point ? @"point" : @"range");
    if (!AsbAXFiniteArray(argument, 2, !point)) return AXFailure(kAXErrorIllegalArgument);
    if (cell) argument = @[argument[1], argument[0]];
    NSDictionary *response = call(operation, nodeID, argument, nil);
    if (![response[@"ok"] boolValue]) return response;
    id result = response[@"value"];
    if (cell) return UiaReference(result) ? AXSuccess(AXValue(@"element", UiaReference(result))) : AXFailure(kAXErrorCannotComplete);
    if (point) return AsbAXFiniteArray(result, 2, YES) ? AXSuccess(AXValue(@"range", result)) : AXFailure(kAXErrorCannotComplete);
    if ([attribute isEqual:@"AXStringForRange"]) return VmAccessibilityIsString(result, ASB_AX_MAX_STRING) ? AXSuccess(result) : AXFailure(kAXErrorCannotComplete);
    if ([attribute isEqual:@"AXBoundsForRange"]) {
        if (![result isKindOfClass:NSArray.class] || ![result count] || [result count] > 65536 || [result count] % 4) return AXFailure(kAXErrorCannotComplete);
        double left = DBL_MAX, top = DBL_MAX, right = -DBL_MAX, bottom = -DBL_MAX;
        for (NSUInteger i = 0; i < [result count]; i += 4) {
            for (NSUInteger j = 0; j < 4; j++) if (!VmAccessibilityIsNumber(result[i + j], j < 2 ? -100000000 : 0, 100000000)) return AXFailure(kAXErrorCannotComplete);
            left = fmin(left, [result[i] doubleValue]); top = fmin(top, [result[i + 1] doubleValue]);
            right = fmax(right, [result[i] doubleValue] + [result[i + 2] doubleValue]);
            bottom = fmax(bottom, [result[i + 1] doubleValue] + [result[i + 3] doubleValue]);
        }
        return AXSuccess(AXValue(@"rect", @[@(left), @(top), @(right - left), @(bottom - top)]));
    }
    if (![result isKindOfClass:NSDictionary.class] || !VmAccessibilityIsString(result[@"text"], ASB_AX_MAX_STRING) ||
        ![result[@"runs"] isKindOfClass:NSArray.class] || [result[@"runs"] count] > 4096) return AXFailure(kAXErrorCannotComplete);
    NSMutableArray *runs = NSMutableArray.array;
    for (id run in result[@"runs"]) {
        if (![run isKindOfClass:NSDictionary.class] || !AsbAXFiniteArray(run[@"range"], 2, YES) ||
            [run[@"range"][0] doubleValue] + [run[@"range"][1] doubleValue] > [result[@"text"] length] ||
            ![run[@"attributes"] isKindOfClass:NSDictionary.class]) return AXFailure(kAXErrorCannotComplete);
        NSDictionary *p = run[@"attributes"];
        NSMutableDictionary *attributes = NSMutableDictionary.dictionary, *font = NSMutableDictionary.dictionary;
        if (VmAccessibilityIsString(p[@"FontName"], ASB_AX_MAX_STRING)) font[@"AXFontFamily"] = p[@"FontName"];
        if (VmAccessibilityIsNumber(p[@"FontSize"], 0, DBL_MAX)) font[@"AXFontSize"] = AXValue(@"double", p[@"FontSize"]);
        if (font.count) attributes[@"AXFont"] = AXValue(@"dictionary", font);
        NSDictionary *keys = @{@"FontWeight": @"AXFontWeight", @"IsItalic": @"AXItalic", @"UnderlineStyle": @"AXUnderlineStyle", @"StrikethroughStyle": @"AXStrikethroughStyle"};
        for (NSString *key in keys) if (VmAccessibilityIsNumber(p[key], INT_MIN, INT_MAX)) attributes[keys[key]] = p[key];
        [runs addObject:@{@"range": run[@"range"], @"attributes": AXValue(@"dictionary", attributes)}];
    }
    return AXSuccess(@{@"$ax": @"attributed", @"value": result[@"text"], @"runs": runs});
}
@end
