/*
 * win_provision.c -- see win_provision.h. Platform-neutral; no Win32 / no Foundation.
 * Content is byte-faithful to src/backend_win/disk_util.c's generate_vhdx_* functions, with the
 * single addition of the AppSandboxSHM install step (harmless no-op on Windows-to-Windows).
 */
#include "win_provision.h"
#include <string.h>

/* ---- base64 of UTF-16LE(pass + "Password") -- matches the unattend <Password PlainText=false>
 * encoding (what CryptBinaryToStringA(BASE64|NOCRLF) produces on Windows). ---- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Decode one UTF-8 sequence at s (NUL-terminated); returns the Unicode code point and
   sets *adv to the bytes consumed. Malformed -> U+FFFD, *adv=1. The continuation-byte
   checks short-circuit on the first non-continuation byte (NUL included), so it never
   reads past the terminator. The input is UTF-8 on both hosts (macOS passes
   NSString.UTF8String; Windows passes WideCharToMultiByte(CP_UTF8,...)). */
static unsigned asb_utf8_next(const unsigned char *s, int *adv) {
    unsigned char c = s[0];
    if (c < 0x80) { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *adv = 2; return ((unsigned)(c & 0x1F) << 6) | (unsigned)(s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *adv = 3; return ((unsigned)(c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (unsigned)(s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *adv = 4; return ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12)
                       | ((unsigned)(s[2] & 0x3F) << 6) | (unsigned)(s[3] & 0x3F);
    }
    *adv = 1; return 0xFFFD;
}

/* ASCII case-insensitive compare (BCP-47 language tags are ASCII); mirrors the Windows
   backend's _wcsicmp lookup so e.g. "de-de" and "de-DE" resolve to the same layout. */
static int asb_ascii_ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static void password_b64(const char *pass, char *out, size_t out_sz) {
    /* Build the UTF-16LE bytes of (pass + "Password"), decoding the UTF-8 input properly
       (each code point -> its real UTF-16 unit(s); astral -> surrogate pair) so non-ASCII
       passwords match Windows' native UTF-16 password encoding. */
    unsigned char buf[1024];
    size_t n = 0;
    const char *parts[2] = { pass, "Password" };
    for (int p = 0; p < 2; p++) {
        const unsigned char *s = (const unsigned char *)parts[p];
        while (*s && n + 2 <= sizeof buf) {
            int adv; unsigned cp = asb_utf8_next(s, &adv); s += adv;
            if (cp <= 0xFFFF) {
                buf[n++] = (unsigned char)(cp & 0xFF);
                buf[n++] = (unsigned char)(cp >> 8);
            } else {
                unsigned v = cp - 0x10000;
                unsigned hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
                buf[n++] = (unsigned char)(hi & 0xFF);
                buf[n++] = (unsigned char)(hi >> 8);
                if (n + 2 <= sizeof buf) {
                    buf[n++] = (unsigned char)(lo & 0xFF);
                    buf[n++] = (unsigned char)(lo >> 8);
                }
            }
        }
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned b0 = buf[i];
        unsigned b1 = (i + 1 < n) ? buf[i + 1] : 0;
        unsigned b2 = (i + 2 < n) ? buf[i + 2] : 0;
        unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        if (o + 4 >= out_sz) break;
        out[o++] = B64[(triple >> 18) & 0x3F];
        out[o++] = B64[(triple >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? B64[(triple >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? B64[triple & 0x3F] : '=';
    }
    out[o] = 0;
}

/* Map a BCP-47 tag to the InputLocale "LCID:KLID" form (same table as lang_to_input_locale). */
static const char *default_input_locale(const char *lang) {
    static const struct { const char *tag, *klid; } map[] = {
        {"en-US","0409:00000409"},{"en-GB","0809:00000809"},{"de-DE","0407:00000407"},
        {"fr-FR","040c:0000040c"},{"fr-CA","0c0c:00001009"},{"es-ES","0c0a:0000040a"},
        {"es-MX","080a:0000080a"},{"it-IT","0410:00000410"},{"pt-BR","0416:00000416"},
        {"pt-PT","0816:00000816"},{"ja-JP","0411:00000411"},{"ko-KR","0412:00000412"},
        {"zh-CN","0804:00000804"},{"zh-TW","0404:00000404"},{"ru-RU","0419:00000419"},
        {"pl-PL","0415:00000415"},{"nl-NL","0413:00000413"},{"sv-SE","041d:0000041d"},
        {"nb-NO","0414:00000414"},{"da-DK","0406:00000406"},{"fi-FI","040b:0000040b"},
        {"cs-CZ","0405:00000405"},{"hu-HU","040e:0000040e"},{"tr-TR","041f:0000041f"},
        {"ar-SA","0401:00000401"},{"he-IL","040d:0000040d"},{"th-TH","041e:0000041e"},
        {"uk-UA","0422:00000422"},{"ro-RO","0418:00000418"},{"el-GR","0408:00000408"},
        {"bg-BG","0402:00020402"},{"hr-HR","041a:0000041a"},{"sk-SK","041b:0000041b"},
        {"sl-SI","0424:00000424"},{"et-EE","0425:00000425"},{"lv-LV","0426:00000426"},
        {"lt-LT","0427:00000427"},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
        if (asb_ascii_ieq(lang, map[i].tag)) return map[i].klid;
    return "0409:00000409";
}

static int xml_escape_text(const char *text, char *out, size_t capacity) {
    size_t used = 0;
    if (!text || !out || !capacity) return 0;
    while (*text) {
        const char *replacement = NULL;
        size_t count;
        switch (*text) {
            case '&': replacement = "&amp;"; break;
            case '<': replacement = "&lt;"; break;
            case '>': replacement = "&gt;"; break;
            case '"': replacement = "&quot;"; break;
            case '\'': replacement = "&apos;"; break;
        }
        count = replacement ? strlen(replacement) : 1;
        if (used + count >= capacity) { out[0] = 0; return 0; }
        memcpy(out + used, replacement ? replacement : text, count);
        used += count;
        text++;
    }
    out[used] = 0;
    return 1;
}

int asb_provision_unattend(FILE *f, const char *vm_name, const char *user, const char *pass,
                           const char *arch, int test_mode, int is_arm64, const char *lang,
                           const char *input_locale) {
    if (!f) return -1;
    char user_xml[1024];
    if (!xml_escape_text(user, user_xml, sizeof user_xml)) return -1;
    char comp[64];   /* up to 15 code points (NetBIOS), each <=4 UTF-8 bytes, + NUL */
    {
        const unsigned char *s = (const unsigned char *)vm_name;
        size_t bi = 0; int cps = 0;
        while (*s && cps < 15) {
            int adv; (void)asb_utf8_next(s, &adv);
            if (bi + (size_t)adv >= sizeof comp) break;
            for (int k = 0; k < adv; k++) comp[bi++] = (char)s[k];
            s += adv; cps++;
        }
        comp[bi] = 0;
    }
    char b64[2048]; password_b64(pass ? pass : "", b64, sizeof b64);
    const char *loc = input_locale && input_locale[0] ? input_locale :
                      default_input_locale(lang ? lang : "en-US");
    char input_xml[512];
    if (!xml_escape_text(loc, input_xml, sizeof input_xml)) return -1;
    loc = input_xml;
    if (!lang) lang = "en-US";

    /* UTF-8 BOM, matching the Windows _wfopen_s("w,ccs=UTF-8"). */
    fputs("\xEF\xBB\xBF", f);
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        "          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        "\n"
        "    <settings pass=\"specialize\">\n"
        "        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        "                   processorArchitecture=\"%s\"\n"
        "                   publicKeyToken=\"31bf3856ad364e35\"\n"
        "                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        "            <ComputerName>%s</ComputerName>\n"
        "        </component>\n"
        "        <component name=\"Microsoft-Windows-Deployment\"\n"
        "                   processorArchitecture=\"%s\"\n"
        "                   publicKeyToken=\"31bf3856ad364e35\"\n"
        "                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        "            <RunSynchronous>\n"
        "                <RunSynchronousCommand wcm:action=\"add\">\n"
        "                    <Order>1</Order>\n"
        "                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        "                </RunSynchronousCommand>\n",
        arch, comp, arch);
    {
        int order = 2;
        fprintf(f,
            "                <RunSynchronousCommand wcm:action=\"add\">\n"
            "                    <Order>%d</Order>\n"
            "                    <Path>bcdedit /set recoveryenabled No</Path>\n"
            "                </RunSynchronousCommand>\n"
            "                <RunSynchronousCommand wcm:action=\"add\">\n"
            "                    <Order>%d</Order>\n"
            "                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
            "                </RunSynchronousCommand>\n", order, order + 1);
        order += 2;
        if (test_mode) {
            fprintf(f,
                "                <RunSynchronousCommand wcm:action=\"add\">\n"
                "                    <Order>%d</Order>\n"
                "                    <Path>bcdedit /set testsigning on</Path>\n"
                "                </RunSynchronousCommand>\n", order++);
        }
        if (is_arm64) {
            static const char *const keys[] = {
                "BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck",
                "BypassStorageCheck", "BypassCPUCheck"
            };
            for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
                fprintf(f,
                    "                <RunSynchronousCommand wcm:action=\"add\">\n"
                    "                    <Order>%d</Order>\n"
                    "                    <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n"
                    "                </RunSynchronousCommand>\n",
                    order++, keys[i]);
        }
    }
    fprintf(f,
        "            </RunSynchronous>\n"
        "        </component>\n"
        "    </settings>\n"
        "\n"
        "    <settings pass=\"oobeSystem\">\n"
        "        <component name=\"Microsoft-Windows-International-Core\"\n"
        "                   processorArchitecture=\"%s\"\n"
        "                   publicKeyToken=\"31bf3856ad364e35\"\n"
        "                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        "            <InputLocale>%s</InputLocale>\n"
        "            <SystemLocale>%s</SystemLocale>\n"
        "            <UILanguage>%s</UILanguage>\n"
        "            <UserLocale>%s</UserLocale>\n"
        "        </component>\n"
        "        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        "                   processorArchitecture=\"%s\"\n"
        "                   publicKeyToken=\"31bf3856ad364e35\"\n"
        "                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        "            <OOBE>\n"
        "                <HideEULAPage>true</HideEULAPage>\n"
        "                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
        "                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
        "                <ProtectYourPC>3</ProtectYourPC>\n"
        "            </OOBE>\n"
        "            <UserAccounts><LocalAccounts>\n"
        "                <LocalAccount wcm:action=\"add\">\n"
        "                    <Name>%s</Name>\n"
        "                    <Group>Administrators</Group>\n"
        "                    <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        "                </LocalAccount>\n"
        "            </LocalAccounts></UserAccounts>\n"
        "            <AutoLogon>\n"
        "                <Enabled>true</Enabled>\n"
        "                <Username>%s</Username>\n"
        "                <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        "                <LogonCount>1</LogonCount>\n"
        "            </AutoLogon>\n"
        "            <FirstLogonCommands>\n"
        "                <SynchronousCommand wcm:action=\"add\">\n"
        "                    <Order>1</Order>\n"
        "                    <Description>Run AppSandbox setup</Description>\n"
        "                    <CommandLine>C:\\Windows\\AppSandbox\\setup.cmd</CommandLine>\n"
        "                    <RequiresUserInput>false</RequiresUserInput>\n"
        "                </SynchronousCommand>\n"
        "            </FirstLogonCommands>\n"
        "        </component>\n"
        "    </settings>\n"
        "</unattend>\n",
        arch, loc, lang, lang, lang, arch, user_xml, b64, user_xml, b64);
    return 0;
}

int asb_provision_setup_cmd(FILE *f) {
    if (!f) return -1;
    fputs(
        "@echo off\r\n"
        "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
        "echo === setup.cmd started === >> \"%LOG%\"\r\n"
        "REM Agent already staged at C:\\Windows\\AppSandbox\\ by the disk builder\r\n"
        "\"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
        "echo === setup.cmd finished === >> \"%LOG%\"\r\n",
        f);
    return 0;
}

int asb_provision_setupcomplete(FILE *f, const char *ssh_msi_name) {
    if (!f) return -1;
    fputs(
        "@echo off\r\n"
        "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
        "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
        "echo === SetupComplete.cmd started === >> \"%LOG%\"\r\n"
        "\r\n"
        "set DRVDIR=%SystemRoot%\\AppSandbox\\drivers\r\n"
        "if not exist \"%DRVDIR%\\AppSandboxVDD.inf\" goto :done\r\n"
        "\r\n"
        "echo [SETUP] Enabling test signing... >> \"%LOG%\"\r\n"
        "bcdedit /set testsigning on >> \"%LOG%\" 2>&1\r\n"
        "\r\n"
        "REM ---- AppSandboxSHM: shared-memory transport PCI driver for QEMU's ivshmem device\r\n"
        "REM PCI\\VEN_1AF4&DEV_1110. Installed FIRST so the transport is up before the VDD/agent connect.\r\n"
        "REM It is test-signed, so trust its .cer with certutil (same as the VDD) then bind it to the\r\n"
        "REM already-enumerated PCI device with `devcon update` -- the SUBSYS-qualified driver outranks\r\n"
        "REM the inbox RAM-controller driver. Same pattern as NetKVM below; Windows-to-Windows has no\r\n"
        "REM 1af4:1110 device, so it is a no-op there.\r\n"
        "if exist \"%DRVDIR%\\AppSandboxSHM.cer\" (\r\n"
        "    echo [SHM] Installing certificate... >> \"%LOG%\"\r\n"
        "    certutil -addstore Root \"%DRVDIR%\\AppSandboxSHM.cer\" >> \"%LOG%\" 2>&1\r\n"
        "    certutil -f -addstore TrustedPublisher \"%DRVDIR%\\AppSandboxSHM.cer\" >> \"%LOG%\" 2>&1\r\n"
        ")\r\n"
        "if exist \"%DRVDIR%\\AppSandboxSHM.inf\" (\r\n"
        "    echo [SHM] Installing transport driver with devcon update... >> \"%LOG%\"\r\n"
        "    \"%DRVDIR%\\devcon.exe\" update \"%DRVDIR%\\AppSandboxSHM.inf\" \"PCI\\VEN_1AF4&DEV_1110&SUBSYS_11001AF4&REV_01\" >> \"%LOG%\" 2>&1\r\n"
        "    echo [SHM] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
        ")\r\n"
        "\r\n"
        "REM ---- VDD (IddCx virtual display) -- its cert (WDKTestCert) then devcon install (Root device)\r\n"
        "if exist \"%DRVDIR%\\AppSandboxVDD.cer\" (\r\n"
        "    echo [VDD] Installing certificate... >> \"%LOG%\"\r\n"
        "    certutil -addstore Root \"%DRVDIR%\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
        "    certutil -f -addstore TrustedPublisher \"%DRVDIR%\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
        ")\r\n"
        "echo [VDD] Installing driver with devcon... >> \"%LOG%\"\r\n"
        "\"%DRVDIR%\\devcon.exe\" install \"%DRVDIR%\\AppSandboxVDD.inf\" Root\\AppSandboxVDD >> \"%LOG%\" 2>&1\r\n"
        "echo [VDD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
        "\r\n"
        "REM Disable display sleep so IDD swap chain stays alive\r\n"
        "powercfg /change monitor-timeout-ac 0\r\n"
        "powercfg /change monitor-timeout-dc 0\r\n"
        "powercfg /change standby-timeout-ac 0\r\n"
        "powercfg /change standby-timeout-dc 0\r\n"
        "echo [PWR] Display sleep disabled >> \"%LOG%\"\r\n"
        "\r\n"
        "REM ---- VAD (Microsoft WHQL-signed -- already trusted, no cert needed) -- devcon install\r\n"
        "if exist \"%DRVDIR%\\AppSandboxVAD.inf\" (\r\n"
        "    echo [VAD] Installing driver with devcon... >> \"%LOG%\"\r\n"
        "    \"%DRVDIR%\\devcon.exe\" install \"%DRVDIR%\\AppSandboxVAD.inf\" Root\\AppSandboxVAD >> \"%LOG%\" 2>&1\r\n"
        "    echo [VAD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
        ")\r\n"
        "\r\n"
        "REM ---- NetKVM (virtio-net NIC) -- Red Hat WHQL-signed (no cert needed). `devcon update` binds\r\n"
        "REM it to the existing virtio-net-pci device (1af4:1000) for NAT networking. netkvmp.exe must sit\r\n"
        "REM beside the INF (the INF CopyFiles-es it). No matching device on Windows-to-Windows -> no-op.\r\n"
        "if exist \"%DRVDIR%\\netkvm.inf\" (\r\n"
        "    echo [NETKVM] Installing virtio-net driver with devcon... >> \"%LOG%\"\r\n"
        "    \"%DRVDIR%\\devcon.exe\" update \"%DRVDIR%\\netkvm.inf\" \"PCI\\VEN_1AF4&DEV_1000&SUBSYS_00011AF4&REV_00\" >> \"%LOG%\" 2>&1\r\n"
        "    echo [NETKVM] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
        ")\r\n"
        "\r\n",
        f);

    if (ssh_msi_name && ssh_msi_name[0]) {
        fprintf(f,
            "REM Install OpenSSH Server\r\n"
            "set SSHDIR=%%SystemRoot%%\\AppSandbox\r\n"
            "if exist \"%%SSHDIR%%\\%s\" (\r\n"
            "    echo [SSH] Installing OpenSSH Server... >> \"%%LOG%%\"\r\n"
            "    msiexec /i \"%%SSHDIR%%\\%s\" /qn /norestart >> \"%%LOG%%\" 2>&1\r\n"
            "    echo [SSH] msiexec exit code: %%errorlevel%% >> \"%%LOG%%\"\r\n"
            "    sc config sshd start= auto >> \"%%LOG%%\" 2>&1\r\n"
            "    net start sshd >> \"%%LOG%%\" 2>&1\r\n"
            "    echo [SSH] Done >> \"%%LOG%%\"\r\n"
            ")\r\n"
            "\r\n",
            ssh_msi_name, ssh_msi_name);
    }

    fputs(
        ":done\r\n"
        "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
        f);
    return 0;
}
