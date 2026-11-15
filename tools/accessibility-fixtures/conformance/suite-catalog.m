#import "suite-common.h"

static NSArray<NSTextCheckingResult *> *matches(NSString *pattern, NSString *text) {
    NSError *error = nil; NSRegularExpression *regex = [NSRegularExpression regularExpressionWithPattern:pattern options:0 error:&error]; ASBRequire(regex != nil, error.localizedDescription);
    return [regex matchesInString:text options:0 range:NSMakeRange(0, text.length)];
}
static NSString *group(NSString *text, NSTextCheckingResult *match, NSUInteger index) { return [text substringWithRange:[match rangeAtIndex:index]]; }
static NSString *category(NSString *name) {
    NSArray *suffixes = @[@[@"ParameterizedAttribute",@"query"], @[@"Attribute",@"attribute"], @[@"Notification",@"notification"], @[@"Subrole",@"subrole"], @[@"Role",@"role"], @[@"Action",@"action"]];
    for (NSArray *pair in suffixes) if ([name hasSuffix:pair[0]]) return pair[1]; return @"constant";
}
static NSString *withoutComments(NSString *text) {
    NSMutableString *result = text.mutableCopy;
    for (NSTextCheckingResult *match in [matches(@"/\\*[\\s\\S]*?\\*/|//[^\\n]*", text) reverseObjectEnumerator]) {
        NSMutableString *replacement = NSMutableString.string; for (NSUInteger i = 0; i < match.range.length; i++) if ([text characterAtIndex:match.range.location + i] == '\n') [replacement appendString:@"\n"];
        [result replaceCharactersInRange:match.range withString:replacement];
    }
    return result;
}
static NSDictionary *location(NSDictionary *value) { NSMutableDictionary *result = [value[@"spellingLoc"] mutableCopy] ?: NSMutableDictionary.dictionary; [result addEntriesFromDictionary:value[@"expansionLoc"] ?: value]; return result; }
static void addEntry(NSMutableDictionary *entries, NSString *name, NSString *kind, NSString *path, NSUInteger line, NSString *declaration, NSString *owner) {
    NSString *key = owner ? [owner stringByAppendingFormat:@".%@", name] : name;
    if (!entries[key]) entries[key] = [@{@"id":key, @"kind":kind, @"symbol":name, @"sources":NSMutableArray.array} mutableCopy];
    NSMutableArray *parts = NSMutableArray.array; for (NSString *part in [declaration componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]) if (part.length) [parts addObject:part];
    NSDictionary *source = @{@"header":path, @"line":@(line), @"declaration":[parts componentsJoinedByString:@" "]};
    if (![entries[key][@"sources"] containsObject:source]) [entries[key][@"sources"] addObject:source];
    if (owner) entries[key][@"owner"] = owner;
}
static NSArray *jsonSequence(NSString *source) {
    NSData *data = [source dataUsingEncoding:NSUTF8StringEncoding]; const unsigned char *bytes = data.bytes;
    NSMutableArray *roots = NSMutableArray.array; NSUInteger start = 0, depth = 0; BOOL string = NO, escape = NO;
    for (NSUInteger i = 0; i < data.length; i++) {
        unsigned char c = bytes[i];
        if (string) { if (escape) escape = NO; else if (c == '\\') escape = YES; else if (c == '"') string = NO; continue; }
        if (c == '"') string = YES;
        else if (c == '{' || c == '[') { if (depth++ == 0) start = i; }
        else if (c == '}' || c == ']') { ASBRequire(depth > 0, @"Malformed compiler JSON"); if (--depth == 0) [roots addObject:ASBParse([data subdataWithRange:NSMakeRange(start, i - start + 1)])]; }
    }
    ASBRequire(depth == 0 && !string, @"Truncated compiler JSON"); return roots;
}
static void visit(NSDictionary *node, NSString *parentPath, NSString *owner, NSString *sdk, NSDictionary *contents, NSMutableDictionary *entries, NSMutableSet *strings) {
    NSDictionary *kinds = @{@"FunctionDecl":@"function", @"VarDecl":@"constant", @"TypedefDecl":@"type", @"EnumDecl":@"enum", @"EnumConstantDecl":@"enum-value", @"ObjCInterfaceDecl":@"interface", @"ObjCProtocolDecl":@"protocol", @"ObjCCategoryDecl":@"category", @"ObjCPropertyDecl":@"property", @"ObjCMethodDecl":@"method"};
    NSString *name = node[@"name"], *kind = kinds[node[@"kind"]]; NSDictionary *loc = location(node[@"loc"] ?: @{});
    NSString *path = [(loc[@"file"] ?: parentPath) stringByResolvingSymlinksInPath]; NSData *source = contents[path ?: @""];
    if (!source || [node[@"isImplicit"] boolValue]) return;
    if (name && kind) {
        if ([node[@"kind"] isEqual:@"VarDecl"]) kind = category(name);
        if ([node[@"kind"] isEqual:@"ObjCCategoryDecl"]) name = [node[@"interface"][@"name"] stringByAppendingFormat:@"(%@)", name];
        NSDictionary *beginLocation = location(node[@"range"][@"begin"] ?: loc), *endLocation = location(node[@"range"][@"end"] ?: loc);
        NSUInteger offset = [loc[@"offset"] unsignedIntegerValue], begin = [beginLocation[@"offset"] unsignedIntegerValue], end = [endLocation[@"offset"] unsignedIntegerValue] + [endLocation[@"tokLen"] unsignedIntegerValue];
        const unsigned char *bytes = source.bytes; NSUInteger line = 1;
        for (NSUInteger i = 0; i < MIN(offset, source.length); i++) if (bytes[i] == '\n') line++;
        if ([@[@"interface",@"protocol",@"category",@"enum"] containsObject:kind]) {
            begin = MIN(offset, source.length); while (begin > 0 && bytes[begin - 1] != '\n') begin--;
            end = MIN(offset, source.length); while (end < source.length && bytes[end] != '\n') end++;
        } else if (![kind isEqual:@"enum-value"]) { NSUInteger cursor = end; while (cursor < source.length && bytes[cursor] != ';') cursor++; if (cursor < source.length) end = cursor + 1; }
        ASBRequire(begin <= end && end <= source.length, @"Invalid compiler source range");
        NSString *declaration = [[NSString alloc] initWithData:[source subdataWithRange:NSMakeRange(begin, end - begin)] encoding:NSUTF8StringEncoding] ?: @"";
        addEntry(entries, name, kind, [path substringFromIndex:sdk.length + 1], line, declaration, owner);
        NSString *key = owner ? [owner stringByAppendingFormat:@".%@", name] : name; NSDictionary *type = node[@"type"] ?: node[@"returnType"];
        if (type) { entries[key][@"type"] = type[@"qualType"]; entries[key][@"resolvedType"] = type[@"desugaredQualType"] ?: type[@"qualType"]; }
        if ([kind isEqual:@"property"]) entries[key][@"readonly"] = node[@"readonly"] ?: @NO;
        if ([node[@"kind"] isEqual:@"VarDecl"] && [name hasPrefix:@"NSAccessibility"] && [entries[key][@"resolvedType"] containsString:@"NSString *"]) [strings addObject:name];
        if ([@[@"interface",@"protocol",@"category"] containsObject:kind]) owner = name;
    }
    for (NSDictionary *child in node[@"inner"]) if (kinds[child[@"kind"]]) visit(child, path, owner, sdk, contents, entries, strings);
}
int ASBCatalog(NSDictionary *options) {
    NSString *sdk = options[@"sdk"] ?: [ASBProcess(@[@"/usr/bin/xcrun",@"--show-sdk-path"], nil, 20, YES)[@"stdout"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    sdk = sdk.stringByResolvingSymlinksInPath;
    NSFileManager *fm = NSFileManager.defaultManager; NSMutableDictionary *headers = NSMutableDictionary.dictionary, *contents = NSMutableDictionary.dictionary, *entries = NSMutableDictionary.dictionary;
    NSArray *folders = @[@[@"ApplicationServices.framework/Frameworks/HIServices.framework/Headers",@"AX"], @[@"AppKit.framework/Headers",@"NSAccessibility"], @[@"Accessibility.framework/Headers",@""]];
    for (NSArray *folder in folders) {
        NSString *directory = [sdk stringByAppendingPathComponent:[@"System/Library/Frameworks" stringByAppendingPathComponent:folder[0]]];
        NSArray *filenames = [[fm contentsOfDirectoryAtPath:directory error:NULL] sortedArrayUsingSelector:@selector(compare:)];
        for (NSString *filename in filenames) if ((![folder[1] length] || [filename hasPrefix:folder[1]]) && [filename hasSuffix:@".h"]) {
            NSString *path = [directory stringByAppendingPathComponent:filename], *relative = [path substringFromIndex:sdk.length + 1];
            NSData *data = [NSData dataWithContentsOfFile:path]; NSString *original = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            headers[relative] = ASBHash(path); contents[path.stringByResolvingSymlinksInPath] = data; NSString *text = withoutComments(original);
            for (NSTextCheckingResult *match in matches(@"(?m)^\\s*#define\\s+(kAX\\w+)\\s+([^\\n]+)", text)) {
                NSString *name = group(text, match, 1), *definition = group(text, match, 2), *kind = category(name);
                NSUInteger line = [[text substringToIndex:match.range.location] componentsSeparatedByString:@"\n"].count;
                addEntry(entries, name, kind, relative, line, group(text, match, 0), nil);
                NSTextCheckingResult *value = [matches(@"CFSTR\\(\"([^\"\\n]+)\"\\)", definition) firstObject];
                if (value) entries[name][@"axName"] = group(definition, value, 1);
                if ([filename containsString:@"TextAttributedString"] || [name hasSuffix:@"TextAttribute"] || [name hasSuffix:@"StringAttribute"]) entries[name][@"surface"] = @"attributed-string-key";
            }
        }
    }
    NSString *temporary = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]; [fm createDirectoryAtPath:temporary withIntermediateDirectories:YES attributes:nil error:NULL];
    @try {
        NSString *sourcePath = [temporary stringByAppendingPathComponent:@"inventory.m"];
        [@"#import <Cocoa/Cocoa.h>\n#import <ApplicationServices/ApplicationServices.h>\n#import <Accessibility/Accessibility.h>\n" writeToFile:sourcePath atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        NSMutableSet *strings = NSMutableSet.set;
        for (NSString *prefix in @[@"NSAccessibility",@"AX"]) {
            NSString *ast = ASBProcess(@[@"/usr/bin/xcrun",@"clang",@"-isysroot",sdk,@"-fsyntax-only",@"-fno-modules",@"-x",@"objective-c",@"-Xclang",@"-ast-dump=json",@"-Xclang",[@"-ast-dump-filter=" stringByAppendingString:prefix],sourcePath], nil, 60, YES)[@"stdout"];
            for (NSDictionary *node in jsonSequence(ast)) visit(node, nil, nil, sdk, contents, entries, strings);
        }
        ASBRequire(![strings containsObject:@"NSAccessibilityRoleDescription"] && ![strings containsObject:@"NSAccessibilityActionDescription"], @"Function included among string constants");
        NSMutableString *resolver = [@"#import <Cocoa/Cocoa.h>\nint main(void) { @autoreleasepool { NSMutableDictionary *values = NSMutableDictionary.dictionary;\n" mutableCopy];
        for (NSString *name in [strings.allObjects sortedArrayUsingSelector:@selector(compare:)]) {
            ASBRequire(matches(@"^NSAccessibility[A-Za-z0-9_]+$", name).count == 1, @"Invalid compiler symbol");
            [resolver appendFormat:@"if (&%@ != NULL) { NSString *value = %@; if (value) values[@\"%@\"] = value; }\n", name, name, name];
        }
        [resolver appendString:@"NSData *data = [NSJSONSerialization dataWithJSONObject:values options:NSJSONWritingSortedKeys error:NULL]; if (!data) return 2; fwrite(data.bytes, 1, data.length, stdout); return 0; } }\n"];
        NSString *resolverPath = [temporary stringByAppendingPathComponent:@"constants.m"], *binary = [temporary stringByAppendingPathComponent:@"constants"];
        [resolver writeToFile:resolverPath atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        ASBProcess(@[@"/usr/bin/xcrun",@"clang",@"-fobjc-arc",@"-Werror=incompatible-pointer-types",@"-Wno-deprecated-declarations",@"-isysroot",sdk,@"-framework",@"Cocoa",resolverPath,@"-o",binary], nil, 60, YES);
        NSDictionary *values = ASBParse([ASBProcess(@[binary], nil, 15, YES)[@"stdout"] dataUsingEncoding:NSUTF8StringEncoding]);
        for (NSString *name in values) entries[name][@"axName"] = values[name];
    } @finally { [fm removeItemAtPath:temporary error:NULL]; }
    ASBRequire(headers.count > 0 && entries.count >= 300, @"SDK inventory unexpectedly small");
    NSDictionary *settings = ASBLoad([sdk stringByAppendingPathComponent:@"SDKSettings.json"]); NSMutableDictionary *sdkInfo = NSMutableDictionary.dictionary, *counts = NSMutableDictionary.dictionary;
    for (NSString *key in @[@"CanonicalName",@"Version",@"MaximumDeploymentTarget"]) sdkInfo[key] = settings[key] ?: NSNull.null;
    NSMutableArray *ordered = NSMutableArray.array;
    for (NSString *key in [entries.allKeys sortedArrayUsingSelector:@selector(compare:)]) { NSDictionary *entry = entries[key]; [ordered addObject:entry]; counts[entry[@"kind"]] = @([counts[entry[@"kind"]] integerValue] + 1); }
    NSDictionary *catalog = @{@"schema":@"asb-ax-catalog-1", @"sdk":sdkInfo, @"scope":@"Public declarations in the listed macOS SDK accessibility headers; runtime provider extensions are reported separately.", @"headers":headers, @"entries":ordered};
    NSString *output = options[@"output"] ?: [ASBDirectory stringByAppendingPathComponent:@"macos-sdk.json"];
    if ([options[@"check"] boolValue]) ASBRequire(ASBEqual(ASBLoad(output), catalog), @"SDK inventory changed; review new or removed declarations"); else ASBSave(output, catalog);
    puts(ASBString(@{@"sdk":sdkInfo, @"entries":@(entries.count), @"kinds":counts}).UTF8String); return 0;
}
