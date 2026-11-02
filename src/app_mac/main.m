#import <Cocoa/Cocoa.h>
#import "AppDelegate.h"
#include "headless.h"
#include <string.h>
#include <unistd.h>
#import <Security/Security.h>
#import <Security/AuthorizationTags.h>

static int launch_elevated_gui(void) {
    AuthorizationRef authorization = NULL;
    OSStatus result = AuthorizationCreate(NULL, NULL, kAuthorizationFlagDefaults, &authorization);
    if (result == errAuthorizationSuccess) {
        AuthorizationItem item = { kAuthorizationRightExecute, 0, NULL, 0 };
        AuthorizationRights rights = { 1, &item };
        result = AuthorizationCopyRights(authorization, &rights, NULL,
            kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights, NULL);
    }
    if (result == errAuthorizationSuccess) {
        char *arguments[] = { "--elevated-gui", NULL };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        result = AuthorizationExecuteWithPrivileges(authorization,
            NSBundle.mainBundle.executablePath.fileSystemRepresentation,
            kAuthorizationFlagDefaults, arguments, NULL);
#pragma clang diagnostic pop
    }
    if (authorization) AuthorizationFree(authorization, kAuthorizationFlagDefaults);
    if (result != errAuthorizationSuccess && result != errAuthorizationCanceled) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"AppSandbox could not start with administrator privileges";
        alert.informativeText = [NSString stringWithFormat:@"Authorization error %d.", (int)result];
        [alert runModal];
    }
    return result == errAuthorizationSuccess ? 0 : 1;
}

int main(int argc, const char *argv[]) {
    /* Headless daemon mode: AppSandbox.app/Contents/MacOS/AppSandbox --headless
       (normal launches pass no args, so the GUI path below is unaffected). */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0 || strcmp(argv[i], "-headless") == 0)
            return run_headless(argc, argv);
    }

    @autoreleasepool {
        BOOL elevatedLaunch = NO;
        for (int i = 1; i < argc; ++i)
            if (strcmp(argv[i], "--elevated-gui") == 0) elevatedLaunch = YES;
        if (geteuid() != 0) {
            if (elevatedLaunch) return 3;
            [NSApplication sharedApplication];
            return launch_elevated_gui();
        }
        if (elevatedLaunch) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            AuthorizationRef launchAuthorization = NULL;
            if (AuthorizationCopyPrivilegedReference(&launchAuthorization, 0) == errAuthorizationSuccess)
                AuthorizationFree(launchAuthorization, kAuthorizationFlagDestroyRights);
#pragma clang diagnostic pop
        }
        if (!asb_host_prepare_user()) return 3;
        NSApplication *app = [NSApplication sharedApplication];

        if (asb_instance_lock_acquire() < 0) {
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            NSAlert *alert = [[NSAlert alloc] init];
            alert.alertStyle = NSAlertStyleInformational;
            alert.messageText = @"AppSandbox is already running";
            alert.informativeText = @"The AppSandbox core is already running. "
                                    @"Use its existing app window or local API.";
            [alert runModal];
            return 0;
        }

        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
