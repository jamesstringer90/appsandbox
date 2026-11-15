#import "suite-common.h"

static void bind(NSMutableDictionary *bindings, NSString *symbol, NSString *facet, NSString *test) {
    if (!bindings[symbol]) bindings[symbol] = NSMutableDictionary.dictionary;
    if (!bindings[symbol][facet]) bindings[symbol][facet] = NSMutableSet.set;
    [bindings[symbol][facet] addObject:test];
}
int ASBCoverage(NSDictionary *options) {
    NSDictionary *catalog = ASBLoad([ASBDirectory stringByAppendingPathComponent:@"macos-sdk.json"]); NSArray *manifest = ASBCases();
    NSMutableDictionary *declarations = NSMutableDictionary.dictionary, *byName = NSMutableDictionary.dictionary, *bindings = NSMutableDictionary.dictionary, *caseHashes = NSMutableDictionary.dictionary;
    NSDictionary *functions = @{@"get":@"AXUIElementCopyAttributeValue", @"set":@"AXUIElementSetAttributeValue", @"settable":@"AXUIElementIsAttributeSettable", @"action":@"AXUIElementPerformAction", @"query":@"AXUIElementCopyParameterizedAttributeValue", @"action-description":@"AXUIElementCopyActionDescription", @"count":@"AXUIElementGetAttributeValueCount", @"slice":@"AXUIElementCopyAttributeValues", @"multiple":@"AXUIElementCopyMultipleAttributeValues", @"attributes":@"AXUIElementCopyAttributeNames", @"actions":@"AXUIElementCopyActionNames", @"queries":@"AXUIElementCopyParameterizedAttributeNames", @"hit":@"AXUIElementCopyElementAtPosition", @"hit-center":@"AXUIElementCopyElementAtPosition"};
    for (NSDictionary *entry in catalog[@"entries"]) { declarations[entry[@"id"]] = entry; if (entry[@"axName"]) { if (!byName[entry[@"axName"]]) byName[entry[@"axName"]] = NSMutableSet.set; [byName[entry[@"axName"]] addObject:entry[@"id"]]; } }
    NSMutableArray *unknownSymbols = NSMutableArray.array;
    for (NSDictionary *test in manifest) {
        NSString *layer = [(test[@"layer"] ?: @"native-behavior") stringByAppendingString:@":"], *name = test[@"id"]; caseHashes[name] = ASBHashValue(test);
        for (NSString *symbol in test[@"symbols"]) if (declarations[symbol]) bind(bindings, symbol, [layer stringByAppendingString:@"declared"], name); else [unknownSymbols addObject:@{@"case":name, @"symbol":symbol}];
        NSMutableArray *operations = [test[@"operations"] mutableCopy];
        for (NSDictionary *step in [(test[@"setup"] ?: @[]) arrayByAddingObjectsFromArray:test[@"after"] ?: @[]]) [operations addObjectsFromArray:step[@"operations"]];
        for (NSString *field in test[@"reads"]) [operations addObject:@{@"kind":@"get", @"name":field}];
        for (NSDictionary *operation in operations) {
            NSString *kind = operation[@"kind"], *function = functions[kind];
            if (function) bind(bindings, function, [layer stringByAppendingString:@"call"], name);
            for (NSString *field in operation[@"name"] ? @[operation[@"name"]] : operation[@"names"] ?: @[]) for (NSString *symbol in byName[field]) bind(bindings, symbol, [layer stringByAppendingString:kind], name);
        }
        if (test[@"observe"]) {
            for (NSString *field in test[@"observe"][@"names"]) for (NSString *symbol in byName[field]) bind(bindings, symbol, [layer stringByAppendingString:@"observe"], name);
            for (NSString *function in @[@"AXObserverCreateWithInfoCallback",@"AXObserverAddNotification",@"AXObserverRemoveNotification",@"AXObserverGetRunLoopSource"]) bind(bindings, function, [layer stringByAppendingString:@"call"], name);
        }
    }
    NSMutableDictionary *results = NSMutableDictionary.dictionary; NSMutableArray *evidence = NSMutableArray.array, *stale = NSMutableArray.array;
    for (NSString *directory in options[@"run"] ?: @[]) {
        NSDictionary *data = ASBLoad([directory stringByAppendingPathComponent:@"result.json"]); [evidence addObject:directory];
        NSString *preflightPath = [directory stringByAppendingPathComponent:@"preflight.json"];
        BOOL sameFixture = [NSFileManager.defaultManager fileExistsAtPath:preflightPath] && [ASBLoad(preflightPath)[@"fixtureRevision"] isEqual:ASBFixtureRevision()];
        NSDictionary *provenance = ASBLoad([directory stringByAppendingPathComponent:@"provenance.json"]);
        BOOL sameRunner = [provenance[@"readerSHA256"] isEqual:ASBHash([ASBDirectory stringByAppendingPathComponent:@"build/ax-probe"])] &&
            [provenance[@"runnerSHA256"] isEqual:ASBHash([ASBDirectory stringByAppendingPathComponent:@"build/ax-suite"])];
        for (NSDictionary *result in data[@"cases"]) {
            NSString *name = result[@"id"];
            if (!sameFixture || !sameRunner || ![result[@"caseSHA256"] isEqual:caseHashes[name]]) { [stale addObject:@{@"run":directory, @"case":name, @"reason":!sameFixture ? @"fixture changed or unverified" : !sameRunner ? @"reader or runner changed or unverified" : @"case changed"}]; continue; }
            if (!results[name]) results[name] = NSMutableArray.array; [results[name] addObject:result[@"status"]];
        }
    }
    NSMutableArray *features = NSMutableArray.array; NSMutableDictionary *assignment = NSMutableDictionary.dictionary, *byKind = NSMutableDictionary.dictionary;
    for (NSDictionary *declaration in catalog[@"entries"]) {
        NSString *symbol = declaration[@"id"], *kind = declaration[@"kind"]; NSMutableDictionary *facets = NSMutableDictionary.dictionary;
        for (NSString *facet in bindings[symbol]) {
            NSMutableArray *observed = NSMutableArray.array; NSArray *identifiers = [[bindings[symbol][facet] allObjects] sortedArrayUsingSelector:@selector(compare:)];
            for (NSString *identifier in identifiers) [observed addObjectsFromArray:results[identifier] ?: @[]];
            BOOL failing = NO; NSMutableDictionary *counts = NSMutableDictionary.dictionary;
            for (NSString *status in observed) { counts[status] = @([counts[status] integerValue] + 1); if ([@[@"FAIL",@"INCOMPLETE",@"INTERRUPTED",@"BASELINE_MISMATCH"] containsObject:status]) failing = YES; }
            NSString *status = !observed.count ? @"UNRUN" : failing ? @"FAILING" : [observed containsObject:@"PASS"] ? @"PASS_OBSERVED" : [observed containsObject:@"NATIVE_OK"] ? @"NATIVE_BASELINE_ONLY" : @"NO_SUCCESSFUL_NATIVE_EXERCISE";
            facets[facet] = @{@"status":status, @"cases":identifiers, @"results":counts};
        }
        NSMutableArray *sources = NSMutableArray.array; for (NSDictionary *source in declaration[@"sources"]) [sources addObject:@{@"header":source[@"header"], @"line":source[@"line"]}];
        NSString *status = facets.count ? @"HAS_CASES" : @"GAP"; assignment[status] = @([assignment[status] integerValue] + 1);
        if (!byKind[kind]) byKind[kind] = NSMutableDictionary.dictionary; byKind[kind][status] = @([byKind[kind][status] integerValue] + 1);
        [features addObject:@{@"id":symbol, @"kind":kind, @"axName":declaration[@"axName"] ?: NSNull.null, @"status":status, @"facets":facets, @"sources":sources}];
    }
    NSDictionary *summary = @{@"declarations":@(features.count), @"cases":@(manifest.count), @"assignment":assignment, @"byKind":byKind};
    NSDictionary *report = @{@"schema":@"asb-ax-coverage-1", @"sdk":catalog[@"sdk"], @"runs":evidence, @"staleEvidence":stale, @"unknownSymbols":unknownSymbols, @"meaning":@"HAS_CASES records assigned exercises, not complete conformance. Each facet requires successful native and production-proxy evidence for the current case. Every unassigned declaration remains a GAP.", @"summary":summary, @"features":features};
    ASBSave(ASBRequired(options, @"output"), report); puts(ASBString(summary).UTF8String); return 0;
}
