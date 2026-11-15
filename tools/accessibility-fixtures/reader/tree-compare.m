#import "../conformance/suite-common.h"

static NSSet *absent(void) { return [NSSet setWithArray:@[@(-25205),@(-25212),@(-25208)]]; }
static BOOL sameKeys(NSDictionary *value, NSArray *keys) { return [[NSSet setWithArray:value.allKeys] isEqual:[NSSet setWithArray:keys]]; }
static NSArray *validateSpec(NSDictionary *spec) {
    ASBRequire(sameKeys(spec,@[@"version",@"cases"]) && [spec[@"version"] isEqual:@1] && [spec[@"cases"] isKindOfClass:NSArray.class] && [spec[@"cases"] count],@"Spec requires version 1 and nonempty cases");
    NSMutableSet *names = NSMutableSet.set, *fields = [NSMutableSet setWithObject:@"AXRole"];
    NSSet *allowed = [NSSet setWithArray:@[@"name",@"match",@"baseline",@"candidate",@"attributes",@"expected",@"subtree"]];
    for (NSDictionary *test in spec[@"cases"]) {
        ASBRequire([[NSSet setWithArray:test.allKeys] isSubsetOfSet:allowed],@"Unknown case option"); NSString *name = test[@"name"];
        ASBRequire([name isKindOfClass:NSString.class] && name.length && ![names containsObject:name],@"Case names must be unique nonempty strings"); [names addObject:name];
        ASBRequire((test[@"match"] != nil) != (test[@"baseline"] != nil || test[@"candidate"] != nil),@"Specify match, or both baseline and candidate selectors");
        for (NSString *side in @[@"baseline",@"candidate"]) {
            NSDictionary *selector = test[side] ?: test[@"match"]; ASBRequire([selector isKindOfClass:NSDictionary.class] && selector.count,@"Missing selector");
            for (NSString *field in selector) { id value = selector[field]; ASBRequire([field hasPrefix:@"AX"] && ([value isKindOfClass:NSString.class] || [value isKindOfClass:NSNumber.class]),@"Selectors require exact scalar AX values"); [fields addObject:field]; }
        }
        NSArray *attributes = test[@"attributes"]; ASBRequire([attributes isKindOfClass:NSArray.class] && attributes.count && [NSSet setWithArray:attributes].count == attributes.count,@"Compared attributes must be nonempty and unique");
        for (NSString *field in attributes) { ASBRequire([field isKindOfClass:NSString.class] && ([field hasPrefix:@"AX"] || [@[@"@actions",@"@queries"] containsObject:field]),@"Invalid compared attribute"); if ([field hasPrefix:@"AX"]) [fields addObject:field]; }
        ASBRequire(!test[@"subtree"] || CFGetTypeID((__bridge CFTypeRef)test[@"subtree"]) == CFBooleanGetTypeID(),@"Subtree must be boolean");
        NSDictionary *expected = test[@"expected"] ?: @{}; ASBRequire([expected isKindOfClass:NSDictionary.class] && [[NSSet setWithArray:expected.allKeys] isSubsetOfSet:[NSSet setWithArray:attributes]],@"Expected keys must be compared attributes");
    }
    return [fields.allObjects sortedArrayUsingSelector:@selector(compare:)];
}
static NSDictionary *indexSnapshot(NSDictionary *snapshot) {
    ASBRequire([snapshot[@"schema"] isEqual:@"asb-ax-tree-1"] && [snapshot[@"readOnly"] isEqual:@YES] && [snapshot[@"trusted"] isEqual:@YES],@"Not a trusted public AX snapshot");
    ASBRequire([snapshot[@"complete"] isEqual:@YES] && [snapshot[@"issues"] isEqual:@[]],@"Incomplete snapshot");
    NSMutableDictionary *index = NSMutableDictionary.dictionary;
    for (NSDictionary *node in snapshot[@"nodes"]) { ASBRequire(node[@"id"] && !index[node[@"id"]],@"Duplicate or missing node identity"); index[node[@"id"]] = node; }
    ASBRequire(index.count && [snapshot[@"roots"] count],@"Empty graph");
    for (NSDictionary *node in index.allValues) {
        ASBRequire([node[@"childrenError"] isEqual:@0] || [absent() containsObject:node[@"childrenError"]],@"Unread child edges");
        for (id child in node[@"children"]) ASBRequire(index[child] != nil,@"Unread child edge");
    }
    NSMutableSet *seen = NSMutableSet.set; NSMutableArray *queue = [snapshot[@"roots"] mutableCopy];
    for (NSUInteger i = 0; i < queue.count; i++) { id key = queue[i]; ASBRequire(index[key] != nil,@"Unread root"); if (![seen containsObject:key]) { [seen addObject:key]; [queue addObjectsFromArray:index[key][@"children"]]; } }
    ASBRequire(seen.count == index.count,@"Unreachable nodes"); return index;
}
static NSDictionary *observation(NSDictionary *node, NSString *field) {
    return [field isEqual:@"@actions"] ? node[@"actions"] ?: @{@"unread":@YES} : [field isEqual:@"@queries"] ? node[@"queries"] ?: @{@"unread":@YES} : node[@"attributes"][field] ?: @{@"unread":@YES};
}
static NSDictionary *normalize(NSDictionary *value, NSDictionary *identities, BOOL allowAbsent) {
    ASBRequire([value isKindOfClass:NSDictionary.class],@"Invalid encoded observation");
    if (sameKeys(value,@[@"ref"])) { ASBRequire(identities[value[@"ref"]] != nil,@"Reference outside selected subtree"); return @{@"ref":identities[value[@"ref"]]}; }
    if (sameKeys(value,@[@"items"])) { NSMutableArray *items = NSMutableArray.array; for (NSDictionary *item in value[@"items"]) [items addObject:normalize(item,identities,allowAbsent)]; return @{@"items":items}; }
    if (sameKeys(value,@[@"error"]) && [absent() containsObject:value[@"error"]] && allowAbsent) return value;
    if (value[@"value"] && [[NSSet setWithArray:value.allKeys] isSubsetOfSet:[NSSet setWithArray:@[@"value",@"type"]]]) return value;
    ASBRequire(NO,[@"Attribute not readable: " stringByAppendingString:ASBString(value)]); return nil;
}
static NSArray *project(NSDictionary *index, NSDictionary *test, NSString *side) {
    NSDictionary *selector = test[side] ?: test[@"match"]; NSMutableArray *matches = NSMutableArray.array;
    for (NSDictionary *node in index.allValues) { BOOL match = YES; for (NSString *field in selector) if (!observation(node,field)[@"value"] || !ASBEqual(observation(node,field)[@"value"],selector[field])) match = NO; if (match) [matches addObject:node]; }
    ASBRequire(matches.count == 1,[NSString stringWithFormat:@"%@ selector matched %lu nodes",side,(unsigned long)matches.count]);
    id root = matches[0][@"id"]; NSMutableArray *keys = [NSMutableArray arrayWithObject:root]; NSMutableDictionary *identities = [NSMutableDictionary dictionaryWithObject:@0 forKey:root]; BOOL subtree = [test[@"subtree"] boolValue];
    if (subtree) for (NSUInteger i = 0; i < keys.count; i++) for (id child in index[keys[i]][@"children"]) if (!identities[child]) { identities[child] = @(keys.count); [keys addObject:child]; }
    NSMutableSet *fields = [NSMutableSet setWithArray:test[@"attributes"]]; if (subtree) [fields addObject:@"AXRole"];
    NSMutableArray *result = NSMutableArray.array;
    for (id key in keys) { NSDictionary *node = index[key]; NSMutableDictionary *attributes = NSMutableDictionary.dictionary, *row = NSMutableDictionary.dictionary;
        for (NSString *field in fields) attributes[field] = normalize(observation(node,field),identities,subtree && ![field isEqual:@"AXRole"]); row[@"attributes"] = attributes;
        if (subtree) { NSMutableArray *children = NSMutableArray.array; for (id child in node[@"children"]) [children addObject:identities[child]]; row[@"children"] = children; } [result addObject:row]; }
    return result;
}
static NSDictionary *compare(NSDictionary *spec, NSDictionary *baseline, NSDictionary *candidate, NSDictionary *after) {
    validateSpec(spec); NSDictionary *beforeIndex, *candidateIndex, *afterIndex = nil;
    @try { beforeIndex = indexSnapshot(baseline); candidateIndex = indexSnapshot(candidate); if (after) { afterIndex = indexSnapshot(after); for (NSString *key in @[@"pid",@"executable",@"processStartedAt"]) ASBRequire(ASBEqual(baseline[key],after[key]),@"Baseline process identity changed"); } }
    @catch (NSException *error) { return @{@"status":@"INCOMPLETE",@"reason":error.reason,@"cases":@[]}; }
    NSMutableArray *results = NSMutableArray.array; NSString *status = @"PASS";
    for (NSDictionary *test in spec[@"cases"]) {
        NSMutableDictionary *result = [@{@"name":test[@"name"]} mutableCopy];
        @try { NSArray *native = project(beforeIndex,test,@"baseline"); if (afterIndex) ASBRequire(ASBEqual(native,project(afterIndex,test,@"baseline")),@"Baseline changed during paired capture");
            for (NSString *field in test[@"expected"]) ASBRequire(native[0][@"attributes"][field][@"value"] && ASBEqual(native[0][@"attributes"][field][@"value"],test[@"expected"][field]),[@"Baseline precondition failed: " stringByAppendingString:field]);
            NSArray *mirrored = project(candidateIndex,test,@"candidate"), *differences = ASBDifferences(native,mirrored,@""); NSMutableArray *changed = NSMutableArray.array;
            for (NSDictionary *diff in [differences subarrayWithRange:NSMakeRange(0,MIN(100,differences.count))]) [changed addObject:@{@"path":diff[@"path"],@"baseline":diff[@"expected"],@"candidate":diff[@"actual"]}];
            [result addEntriesFromDictionary:@{@"status":differences.count ? @"FAIL" : @"PASS",@"comparedNodes":@[@(native.count),@(mirrored.count)],@"attributes":test[@"attributes"],@"differences":changed,@"differencesTruncated":@(differences.count > 100)}];
        } @catch (NSException *error) { result[@"status"] = @"INCOMPLETE"; result[@"reason"] = error.reason; }
        if ([result[@"status"] isEqual:@"INCOMPLETE"] || ([status isEqual:@"PASS"] && [result[@"status"] isEqual:@"FAIL"])) status = result[@"status"]; [results addObject:result];
    }
    return @{@"status":status,@"scope":@"read-only public AX parity",@"baselineBracketed":@(after != nil),@"baselinePID":baseline[@"pid"] ?: NSNull.null,@"candidatePID":candidate[@"pid"] ?: NSNull.null,@"sameProcess":@(ASBEqual(baseline[@"pid"],candidate[@"pid"])),@"cases":results};
}
static NSDictionary *capture(NSString *reader, NSNumber *pid, NSArray *fields, NSString *window, double timeout, NSInteger maxNodes) {
    double start = NSProcessInfo.processInfo.systemUptime; NSMutableDictionary *snapshot;
    @try { NSDictionary *process = ASBProcess(@[reader,pid.stringValue,@(maxNodes).stringValue,@(timeout).stringValue,[fields componentsJoinedByString:@","],window],nil,timeout + 1,NO); snapshot = ASBParse([process[@"stdout"] dataUsingEncoding:NSUTF8StringEncoding]);
        if ([process[@"status"] intValue] != 0) { snapshot[@"complete"] = @NO; if (!snapshot[@"issues"]) snapshot[@"issues"] = NSMutableArray.array; [snapshot[@"issues"] addObject:[@"Reader exit " stringByAppendingString:[process[@"status"] description]]]; }
    } @catch (NSException *error) { snapshot = [@{@"schema":@"asb-ax-tree-1",@"complete":@NO,@"issues":@[error.reason],@"nodes":@[],@"pid":pid} mutableCopy]; }
    snapshot[@"commandElapsedMs"] = @((NSProcessInfo.processInfo.systemUptime - start) * 1000); return snapshot;
}
int ASBTreeCommand(NSString *command, NSDictionary *options) {
    if (options[@"output"]) ASBRequire(![NSFileManager.defaultManager fileExistsAtPath:options[@"output"]],@"Output file already exists");
    double timeout = options[@"timeout"] ? [options[@"timeout"] doubleValue] : 10; NSInteger maxNodes = options[@"max-nodes"] ? [options[@"max-nodes"] integerValue] : 8192;
    ASBRequire(timeout > 0 && timeout <= 120 && maxNodes > 0 && maxNodes <= 65536,@"Invalid capture limits");
    NSString *reader = [ASBDirectory.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"reader/build/ax-tree"];
    if ([command isEqual:@"tree-snapshot"]) {
        NSArray *fields = [(options[@"attributes"] ?: @"AXRole,AXSubrole,AXTitle,AXDescription,AXIdentifier,AXDOMIdentifier,AXValue,AXEnabled,AXFocused,AXSelected,AXExpanded,AXDisclosing,AXMinValue,AXMaxValue,AXSelectedTextRange,AXNumberOfCharacters,AXPosition,AXSize") componentsSeparatedByString:@","];
        NSDictionary *snapshot = capture(reader,@([ASBRequired(options,@"pid") intValue]),fields,options[@"window"] ?: @"",timeout,maxNodes); ASBSave(ASBRequired(options,@"output"),snapshot);
        puts(ASBString(@{@"complete":snapshot[@"complete"],@"nodes":@([snapshot[@"nodes"] count]),@"elapsedMs":snapshot[@"commandElapsedMs"]}).UTF8String); return [snapshot[@"complete"] boolValue] ? 0 : 2;
    }
    NSDictionary *spec = ASBLoad(ASBRequired(options,@"spec")); NSArray *fields = validateSpec(spec); NSMutableDictionary *result;
    if ([command isEqual:@"tree-pair"]) {
        NSString *output = ASBRequired(options,@"output-dir"); ASBRequire(![NSFileManager.defaultManager fileExistsAtPath:output],@"Output directory already exists"); NSError *error = nil; ASBRequire([NSFileManager.defaultManager createDirectoryAtPath:output withIntermediateDirectories:YES attributes:nil error:&error],error.localizedDescription); ASBSave([output stringByAppendingPathComponent:@"spec.json"],spec);
        NSNumber *baselinePID = @([ASBRequired(options,@"baseline-pid") intValue]), *candidatePID; NSString *candidateWindow = options[@"candidate-window"] ?: @"", *vmPath = nil; ASBAPI *api = nil;
        if (options[@"vm"]) { ASBRequire(!options[@"candidate-pid"],@"Choose --vm or --candidate-pid"); api = ASBAPI.new; vmPath = [api vmPath:options[@"vm"]]; NSDictionary *state = [api request:@"GET" path:vmPath];
            if (![state[@"running"] boolValue]) [api request:@"POST" path:[vmPath stringByAppendingString:@"/start"]];
            double budget = options[@"vm-timeout"] ? [options[@"vm-timeout"] doubleValue] : 180; ASBRequire(budget > 0 && budget <= 600,@"Invalid VM startup timeout"); double deadline = NSProcessInfo.processInfo.systemUptime + budget;
            while (![[api request:@"GET" path:[vmPath stringByAppendingString:@"/display"]][@"ready"] boolValue]) { ASBRequire(NSProcessInfo.processInfo.systemUptime < deadline,@"VM display readiness deadline"); [NSThread sleepForTimeInterval:1]; }
            [api request:@"POST" path:[vmPath stringByAppendingString:@"/display"]]; candidatePID = api.discovery[@"pid"]; candidateWindow = [state[@"name"] stringByAppendingString:@" — Display"];
            ASBRequire(![baselinePID isEqual:candidatePID],@"Native baseline must be a different process from the VM viewer"); ASBSave([output stringByAppendingPathComponent:@"vm-before.json"],[api request:@"GET" path:vmPath]);
        } else candidatePID = @([ASBRequired(options,@"candidate-pid") intValue]);
        NSArray *names = @[@"baseline",@"candidate",@"baseline-after"], *pids = @[baselinePID,candidatePID,baselinePID], *windows = @[options[@"baseline-window"] ?: @"",candidateWindow,options[@"baseline-window"] ?: @""]; NSMutableArray *captures = NSMutableArray.array, *times = NSMutableArray.array;
        for (NSUInteger i = 0; i < names.count; i++) { NSDictionary *snapshot = capture(reader,pids[i],fields,windows[i],timeout,maxNodes); [captures addObject:snapshot]; [times addObject:snapshot[@"commandElapsedMs"]]; ASBSave([output stringByAppendingPathComponent:[names[i] stringByAppendingString:@".json"]],snapshot); }
        result = [compare(spec,captures[0],captures[1],captures[2]) mutableCopy]; result[@"captureMs"] = times; result[@"readerSHA256"] = ASBHash(reader);
        NSMutableDictionary *hashes = NSMutableDictionary.dictionary; for (NSString *file in @[@"spec.json",@"baseline.json",@"candidate.json",@"baseline-after.json"]) hashes[file] = ASBHash([output stringByAppendingPathComponent:file]); result[@"sha256"] = hashes;
        if (api) { NSDictionary *state = [api request:@"GET" path:vmPath]; ASBSave([output stringByAppendingPathComponent:@"vm-after.json"],state); result[@"vm"] = @{@"id":state[@"id"],@"name":state[@"name"],@"osType":state[@"osType"]}; if (![state[@"running"] boolValue] || ![state[@"displayOpen"] boolValue] || ![state[@"agentOnline"] boolValue]) { result[@"status"] = @"INCOMPLETE"; result[@"reason"] = @"VM or viewer became unavailable during capture"; } }
        ASBSave([output stringByAppendingPathComponent:@"result.json"],result);
    } else { result = [compare(spec,ASBLoad(ASBRequired(options,@"baseline")),ASBLoad(ASBRequired(options,@"candidate")),options[@"baseline-after"] ? ASBLoad(options[@"baseline-after"]) : nil) mutableCopy]; if (options[@"output"]) ASBSave(options[@"output"],result); }
    puts(ASBString(result).UTF8String); return [result[@"status"] isEqual:@"PASS"] ? 0 : [result[@"status"] isEqual:@"FAIL"] ? 1 : 2;
}
void ASBTreeSelfTest(void) {
    NSDictionary *source = @{@"schema":@"asb-ax-tree-1",@"readOnly":@YES,@"trusted":@YES,@"complete":@YES,@"issues":@[],@"pid":@10,@"executable":@"fixture",@"processStartedAt":@"start",@"roots":@[@0],@"nodes":@[@{@"id":@0,@"children":@[@1],@"childrenError":@0,@"attributes":@{@"AXRole":@{@"value":@"AXWindow"},@"AXTitle":@{@"value":@"Fixture"}}},@{@"id":@1,@"children":@[],@"childrenError":@(-25205),@"attributes":@{@"AXRole":@{@"value":@"AXCheckBox"},@"AXTitle":@{@"value":@"Meal"},@"AXValue":@{@"value":@0},@"AXEnabled":@{@"value":@YES}},@"actions":@{@"value":@[@"AXPress"]}}]};
    NSDictionary *spec = @{@"version":@1,@"cases":@[@{@"name":@"checkbox",@"match":@{@"AXTitle":@"Meal"},@"attributes":@[@"AXRole",@"AXValue",@"AXEnabled",@"@actions"],@"expected":@{@"AXValue":@0}}]};
    ASBRequire([compare(spec,source,source,source)[@"status"] isEqual:@"PASS"],@"Comparator rejected exact baseline");
    NSMutableDictionary *changed = ASBParse(ASBJSON(source)); changed[@"nodes"][1][@"attributes"][@"AXEnabled"] = @{@"value":@1}; ASBRequire([compare(spec,source,changed,nil)[@"status"] isEqual:@"FAIL"],@"Boolean/number distinction lost");
    for (NSDictionary *unread in @[@{@"error":@(-25205)},@{@"error":@(-25204)},@{@"redacted":@YES},@{@"unread":@YES},@{@"unsupportedType":@90},@{@"externalReference":@YES}]) { changed = ASBParse(ASBJSON(source)); changed[@"nodes"][1][@"attributes"][@"AXValue"] = unread; ASBRequire([compare(spec,changed,changed,nil)[@"status"] isEqual:@"INCOMPLETE"],@"Unread equal observations passed"); }
    changed = ASBParse(ASBJSON(source)); changed[@"nodes"][1][@"attributes"][@"AXValue"] = @{@"value":@1}; ASBRequire([compare(spec,source,changed,nil)[@"status"] isEqual:@"FAIL"] && [compare(spec,source,source,changed)[@"status"] isEqual:@"INCOMPLETE"],@"Value loss or changing baseline passed");
    changed = ASBParse(ASBJSON(source)); changed[@"nodes"][1][@"actions"] = @{@"value":@[]}; ASBRequire([compare(spec,source,changed,nil)[@"status"] isEqual:@"FAIL"],@"Action loss passed");
    for (NSString *failure in @[@"missing",@"ambiguous",@"truncated",@"empty",@"broken",@"duplicate",@"restart"]) {
        changed = ASBParse(ASBJSON(source));
        if ([failure isEqual:@"missing"]) changed[@"nodes"][1][@"attributes"][@"AXTitle"] = @{@"value":@"Other"};
        if ([failure isEqual:@"ambiguous"]) changed[@"nodes"][0][@"attributes"][@"AXTitle"] = @{@"value":@"Meal"};
        if ([failure isEqual:@"truncated"]) changed[@"complete"] = @NO;
        if ([failure isEqual:@"empty"]) changed[@"nodes"] = @[];
        if ([failure isEqual:@"broken"]) [changed[@"nodes"] removeLastObject];
        if ([failure isEqual:@"duplicate"]) [changed[@"nodes"] addObject:changed[@"nodes"][0]];
        if ([failure isEqual:@"restart"]) changed[@"processStartedAt"] = @"restart";
        ASBRequire([compare(spec,source,changed,changed)[@"status"] isEqual:@"INCOMPLETE"],[@"Invalid snapshot passed: " stringByAppendingString:failure]);
    }
    NSDictionary *subtreeSpec = @{@"version":@1,@"cases":@[@{@"name":@"tree",@"match":@{@"AXRole":@"AXWindow"},@"subtree":@YES,@"attributes":@[@"AXRole",@"AXTitle",@"AXValue",@"AXSelectedChildren"]}]};
    NSMutableDictionary *native = ASBParse(ASBJSON(source)); native[@"nodes"][0][@"attributes"][@"AXValue"] = @{@"error":@(-25205)}; native[@"nodes"][0][@"attributes"][@"AXSelectedChildren"] = @{@"items":@[@{@"ref":@1}]}; native[@"nodes"][1][@"attributes"][@"AXSelectedChildren"] = @{@"error":@(-25205)}; native[@"nodes"][1][@"children"] = @[@0];
    changed = ASBParse(ASBJSON(native)); changed[@"nodes"][0][@"id"] = @50; changed[@"nodes"][1][@"children"] = @[@50]; changed[@"roots"] = @[@50];
    ASBRequire([compare(subtreeSpec,native,changed,nil)[@"status"] isEqual:@"PASS"],@"Local node renumbering changed comparison");
    changed[@"nodes"][0][@"attributes"][@"AXSelectedChildren"] = @{@"items":@[@{@"ref":@50}]}; ASBRequire([compare(subtreeSpec,native,changed,nil)[@"status"] isEqual:@"FAIL"],@"Reference alias change passed");
    changed = ASBParse(ASBJSON(native)); native[@"nodes"][0][@"children"] = @[@1,@0]; changed[@"nodes"][0][@"children"] = @[@0,@1]; ASBRequire([compare(subtreeSpec,native,changed,nil)[@"status"] isEqual:@"FAIL"],@"Child order change passed");
    for (NSDictionary *bad in @[@{@"version":@1,@"cases":@[]},@{@"version":@1,@"cases":@[@{@"name":@"typo",@"attribute":@[@"AXValue"]}]}]) { BOOL rejected = NO; @try { validateSpec(bad); } @catch (NSException *error) { (void)error; rejected = YES; } ASBRequire(rejected,@"Invalid spec passed"); }
}
