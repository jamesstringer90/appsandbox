#import <Foundation/Foundation.h>

extern NSString *ASBDirectory;
void ASBRequire(BOOL condition, NSString *message);
id ASBLoad(NSString *path);
id ASBParse(NSData *data);
NSData *ASBJSON(id value);
NSString *ASBString(id value);
void ASBSave(NSString *path, id value);
NSString *ASBHash(NSString *path);
NSString *ASBHashValue(id value);
NSString *ASBFixtureRevision(void);
NSString *ASBQuote(NSString *value);
NSDictionary *ASBProcess(NSArray<NSString *> *arguments, NSData *input, double timeout, BOOL check);
NSDictionary *ASBOptions(NSArray<NSString *> *arguments);
NSString *ASBRequired(NSDictionary *options, NSString *name);
NSArray *ASBDifferences(id expected, id actual, NSString *path);
NSArray *ASBResultDifferences(id expected, id actual, NSString *path);
BOOL ASBEqual(id left, id right);
NSArray *ASBCases(void);
id ASBStable(id value);
NSArray *ASBCompared(NSDictionary *test, NSArray *results);

@interface ASBAPI : NSObject
@property(readonly) NSDictionary *discovery;
@property(readonly) NSString *support;
- (id)request:(NSString *)method path:(NSString *)path;
- (NSString *)vmPath:(NSString *)name;
- (NSDictionary *)status:(NSString *)name;
@end

int ASBRun(NSDictionary *options);
int ASBStressRun(NSDictionary *options);
int ASBCoverage(NSDictionary *options);
int ASBCatalog(NSDictionary *options);
int ASBBuildHost(NSDictionary *options);
int ASBVerifyHost(NSString *app);
int ASBPrepare(NSDictionary *options);
int ASBTreeCommand(NSString *command, NSDictionary *options);
int ASBSelfTest(void);
void ASBTreeSelfTest(void);
