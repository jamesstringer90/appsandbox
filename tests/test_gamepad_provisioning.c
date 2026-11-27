#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tools/provision/win_provision.h"
#include "../src/backend_win/gamepad_provision.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
        g_failures++; \
    } \
} while (0)

static char *read_temp_script(FILE *f)
{
    long sz;
    char *buf;
    fflush(f);
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)calloc(1, (size_t)sz + 1);
    if (buf && sz > 0)
        fread(buf, 1, (size_t)sz, f);
    return buf;
}

/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-004::REQ-011 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-004::REQ-014 */
static void test_provision_scripts_hash_and_sync_decisions(void)
{
    FILE *f = NULL;
    char *content = NULL;
    uint8_t digest[32];
    char hex[65];
    int i;

    for (i = 0; i < 32; i++) digest[i] = (uint8_t)i;
    asb_sha256_to_hex(digest, hex);
    CHECK(strlen(hex) == 64, "hex output must be 64 chars");
    CHECK(strncmp(hex, "000102030405060708090a0b0c0d0e0f", 32) == 0, "hex encoding matches bytes");

    /* setup.cmd copies appsandbox-gamepad.exe in media mode */
    CHECK(tmpfile_s(&f) == 0 && f != NULL, "tmpfile for setup.cmd");
    if (f) {
        CHECK(asb_provision_setup_cmd(f, 1, 1) == 0, "asb_provision_setup_cmd succeeds");
        content = read_temp_script(f);
        CHECK(content && strstr(content, "appsandbox-gamepad.exe") != NULL,
              "setup.cmd must copy appsandbox-gamepad.exe");
        free(content);
        fclose(f);
    }

    /* SetupComplete.cmd trusts AppSandboxVGD .cer certs and registers INFs via pnputil */
    f = NULL;
    CHECK(tmpfile_s(&f) == 0 && f != NULL, "tmpfile for SetupComplete.cmd");
    if (f) {
        CHECK(asb_provision_setupcomplete(f, NULL, 0) == 0, "asb_provision_setupcomplete succeeds");
        content = read_temp_script(f);
        CHECK(content && strstr(content, "AppSandboxVGD_XUSB.cer") != NULL,
              "SetupComplete.cmd must trust AppSandboxVGD_XUSB.cer");
        CHECK(content && strstr(content, "AppSandboxVGD_HID.cer") != NULL,
              "SetupComplete.cmd must trust AppSandboxVGD_HID.cer");
        CHECK(content && strstr(content, "pnputil /add-driver") != NULL,
              "SetupComplete.cmd must invoke pnputil /add-driver");
        CHECK(content && strstr(content, "AppSandboxVGD_XUSB.inf") != NULL,
              "SetupComplete.cmd must register AppSandboxVGD_XUSB.inf");
        CHECK(content && strstr(content, "AppSandboxVGD_HID.inf") != NULL,
              "SetupComplete.cmd must register AppSandboxVGD_HID.inf");
        free(content);
        fclose(f);
    }
}

static void test_preboot_sync_skip_and_nonblocking_fallback(void)
{
    const char *h1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *h2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    CHECK(asb_gamepad_sync_needed("", h1) == 1, "empty recorded hash triggers sync");
    CHECK(asb_gamepad_sync_needed(NULL, h1) == 1, "NULL recorded hash triggers sync");
    CHECK(asb_gamepad_sync_needed(h1, h2) == 1, "changed payload hash triggers sync");
    CHECK(asb_gamepad_sync_needed(h1, h1) == 0, "matching hash skips sync");
    CHECK(asb_gamepad_sync_needed(h1, "") == 0, "empty current hash safely skips sync");
    CHECK(asb_gamepad_sync_needed("", NULL) == 0, "NULL current hash safely skips sync");
}

int main(void)
{
    test_provision_scripts_hash_and_sync_decisions();
    test_preboot_sync_skip_and_nonblocking_fallback();
    if (g_failures == 0) {
        printf("PASS: test_gamepad_provisioning\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", g_failures);
    return 1;
}
