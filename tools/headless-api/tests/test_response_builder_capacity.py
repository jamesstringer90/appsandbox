"""Compile the production headless response builder in a Windows isolation harness.

This test extracts the RespBuild helpers and send_vm_response directly from
headless.c, substitutes only the daemon-facing dependencies, and injects
controlled heap failures. It does not start AppSandbox or touch vms.cfg.
It checks the production VmInstance field types, then uses a response-only
synthetic variant with an enlarged selector to drive the builder above its
limits; no real VM can have a selector of that size.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src" / "app_win" / "headless.c"
CORE_HEADER = ROOT / "src" / "backend_win" / "asb_core.h"
VSWHERE = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")


PREAMBLE = r"""
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "hcs_vm.h"

#define ASB_MAX_VMS @ASB_MAX_VMS@
#define HARNESS_SELECTOR_CAP 1100001

typedef ULONGLONG HTTP_REQUEST_ID;
typedef void *AsbVm;

/* Keep compile-time checks against the production VmInstance definition,
   then use a response-only test variant below. The enlarged selector is
   synthetic input to force the builder past its capacities; a real VM's
   selector remains INTERNAL_SWITCH_CAP (255 stored characters). */
typedef char real_vm_unique_id_matches_uint64[
    sizeof(((VmInstance *)0)->unique_id) == sizeof(UINT64) ? 1 : -1];
typedef char real_vm_selector_matches_shared_cap[
    sizeof(((VmInstance *)0)->internal_switch) ==
    INTERNAL_SWITCH_CAP * sizeof(wchar_t) ? 1 : -1];

typedef struct {
    UINT64 unique_id;
    wchar_t name[256];
    wchar_t os_type[32];
    wchar_t gpu_id[512];
    wchar_t gpu_name[256];
    wchar_t net_adapter[256];
    wchar_t internal_switch[HARNESS_SELECTOR_CAP];
    BOOL internal_switch_invalid;
    BOOL running;
    BOOL agent_online;
    BOOL install_complete;
    BOOL building_vhdx;
    volatile BOOL shutdown_requested;
    int vhdx_progress;
    volatile BOOL ssh_key_deployed;
    volatile int ssh_state;
    DWORD ssh_port;
    DWORD ram_mb;
    DWORD hdd_gb;
    DWORD cpu_cores;
    int gpu_mode;
    int network_mode;
} ResponseCapacityVmInstance;
#define VmInstance ResponseCapacityVmInstance

static VmInstance g_vm;
static int g_heap_calls;
static int g_response_alloc_count;
static SIZE_T g_response_alloc_sizes[4];
static int g_fail_heap_call;
static SIZE_T g_fail_heap_size;
static int g_json_calls;
static int g_error_calls;
static USHORT g_last_status;
static char g_error_code[64];
static char *g_last_body;

static LPVOID harness_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes);
static BOOL harness_heap_free(HANDLE heap, DWORD flags, LPVOID block);
#define HeapAlloc(heap, flags, bytes) harness_heap_alloc((heap), (flags), (bytes))
#define HeapFree(heap, flags, block) harness_heap_free((heap), (flags), (block))

static const char *derive_state(VmInstance *v);
static BOOL display_is_open(UINT64 vm_id);
static void asb_vm_disk_directory(AsbVm vm, wchar_t *out, size_t out_len);
static int asb_vm_count(void);
static AsbVm asb_vm_get(int index);
static VmInstance *asb_vm_instance(AsbVm vm);
static void send_json(HTTP_REQUEST_ID id, USHORT status,
                      const char *reason, const char *body);
static void send_err(HTTP_REQUEST_ID id, USHORT status,
                     const char *reason, const char *code, const char *msg);
"""


POSTAMBLE = r"""
static LPVOID harness_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    LPVOID block;
    (void)heap;
    g_heap_calls++;
    if (flags & HEAP_ZERO_MEMORY) {
        if (g_response_alloc_count < (int)ARRAYSIZE(g_response_alloc_sizes))
            g_response_alloc_sizes[g_response_alloc_count] = bytes;
        g_response_alloc_count++;
    }
    if ((g_fail_heap_call && g_heap_calls == g_fail_heap_call) ||
        (g_fail_heap_size && bytes == g_fail_heap_size))
        return NULL;
    block = (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes) : malloc(bytes);
    return block;
}

static BOOL harness_heap_free(HANDLE heap, DWORD flags, LPVOID block)
{
    (void)heap;
    (void)flags;
    free(block);
    return TRUE;
}

static const char *derive_state(VmInstance *v)
{
    (void)v;
    return "offline";
}

static BOOL display_is_open(UINT64 vm_id)
{
    (void)vm_id;
    return FALSE;
}

static void asb_vm_disk_directory(AsbVm vm, wchar_t *out, size_t out_len)
{
    (void)vm;
    if (out_len)
        out[0] = L'\0';
}

static int asb_vm_count(void)
{
    return 1;
}

static AsbVm asb_vm_get(int index)
{
    return index == 0 ? (AsbVm)&g_vm : NULL;
}

static VmInstance *asb_vm_instance(AsbVm vm)
{
    return (VmInstance *)vm;
}

static void send_json(HTTP_REQUEST_ID id, USHORT status,
                      const char *reason, const char *body)
{
    size_t len;
    (void)id;
    (void)reason;
    len = strlen(body);
    free(g_last_body);
    g_last_body = (char *)malloc(len + 1);
    if (g_last_body)
        memcpy(g_last_body, body, len + 1);
    g_last_status = status;
    g_json_calls++;
}

static void send_err(HTTP_REQUEST_ID id, USHORT status,
                     const char *reason, const char *code, const char *msg)
{
    (void)id;
    (void)reason;
    (void)msg;
    g_last_status = status;
    strncpy_s(g_error_code, sizeof(g_error_code), code, _TRUNCATE);
    g_error_calls++;
}

static void reset_capture(void)
{
    g_heap_calls = 0;
    g_response_alloc_count = 0;
    ZeroMemory(g_response_alloc_sizes, sizeof(g_response_alloc_sizes));
    g_fail_heap_call = 0;
    g_fail_heap_size = 0;
    g_json_calls = 0;
    g_error_calls = 0;
    g_last_status = 0;
    g_error_code[0] = '\0';
    free(g_last_body);
    g_last_body = NULL;
}

static int fill_selector(size_t chars)
{
    if (chars + 1 > ARRAYSIZE(g_vm.internal_switch))
        return 0;
    wmemset(g_vm.internal_switch, L'x', chars);
    g_vm.internal_switch[chars] = L'\0';
    return 1;
}

static int fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    free(g_last_body);
    return 1;
}

int main(int argc, char **argv)
{
    const size_t first_capacity = (size_t)ASB_MAX_VMS * 16384;
    const size_t retry_capacity = first_capacity * 2;

    if (argc != 2)
        return fail("expected one scenario name");
    wcscpy_s(g_vm.name, ARRAYSIZE(g_vm.name), L"harness-vm");
    wcscpy_s(g_vm.os_type, ARRAYSIZE(g_vm.os_type), L"Windows");

    if (strcmp(argv[1], "grow") == 0) {
        const size_t chars = 600000;
        const char *field = "\"internalSwitch\":\"";
        const char *value;
        size_t i;
        if (!fill_selector(chars)) return fail("selector did not fit the test VM");
        reset_capture();
        send_vm_response(1, NULL);
        if (g_last_status != 200 || g_json_calls != 1 || g_error_calls != 0)
            return fail("growth scenario did not return one successful JSON response");
        if (g_response_alloc_count != 2 ||
            g_response_alloc_sizes[0] != first_capacity ||
            g_response_alloc_sizes[1] != retry_capacity)
            return fail("growth scenario did not allocate the initial and doubled capacities");
        value = strstr(g_last_body ? g_last_body : "", field);
        if (!value) return fail("response omitted the internalSwitch field");
        value += strlen(field);
        for (i = 0; i < chars; i++)
            if (value[i] != 'x') return fail("selector was truncated or altered during growth");
        if (value[chars] != '\"') return fail("selector closing quote is missing after growth");
        if (fwrite(g_last_body, 1, strlen(g_last_body), stdout) != strlen(g_last_body))
            return fail("could not write response body");
        free(g_last_body);
        return 0;
    }

    if (strcmp(argv[1], "single-grow") == 0) {
        const size_t chars = 600000;
        const char *field = "\"internalSwitch\":\"";
        const char *value;
        size_t i;
        if (!fill_selector(chars)) return fail("selector did not fit the test VM variant");
        reset_capture();
        send_vm_response(1, &g_vm);
        if (g_last_status != 200 || g_json_calls != 1 || g_error_calls != 0)
            return fail("single-object growth did not return one successful JSON response");
        if (g_response_alloc_count != 2 ||
            g_response_alloc_sizes[0] != first_capacity ||
            g_response_alloc_sizes[1] != retry_capacity)
            return fail("single-object growth did not allocate initial and doubled capacities");
        if (!g_last_body || g_last_body[0] != '{' ||
            strstr(g_last_body, "\"vms\":[") != NULL)
            return fail("single-object branch did not produce an object response");
        value = strstr(g_last_body, field);
        if (!value) return fail("single-object response omitted the internalSwitch field");
        value += strlen(field);
        for (i = 0; i < chars; i++)
            if (value[i] != 'x') return fail("single selector was truncated or altered during growth");
        if (value[chars] != '\"') return fail("single selector closing quote is missing after growth");
        if (fwrite(g_last_body, 1, strlen(g_last_body), stdout) != strlen(g_last_body))
            return fail("could not write single-object response body");
        free(g_last_body);
        return 0;
    }

    if (strcmp(argv[1], "too-large") == 0) {
        if (!fill_selector(1100000)) return fail("large selector did not fit the test VM");
        reset_capture();
        send_vm_response(1, NULL);
        if (g_last_status != 500 || g_json_calls != 0 || g_error_calls != 1 ||
            strcmp(g_error_code, "too_large") != 0)
            return fail("final serialization exhaustion did not return too_large");
        if (g_response_alloc_count != 2 ||
            g_response_alloc_sizes[0] != first_capacity ||
            g_response_alloc_sizes[1] != retry_capacity)
            return fail("too-large scenario did not exhaust both response capacities");
        printf("{\"status\":%u,\"code\":\"%s\"}",
               g_last_status, g_error_code);
        return 0;
    }

    if (strcmp(argv[1], "alloc-first") == 0) {
        if (!fill_selector(1)) return fail("selector initialization failed");
        reset_capture();
        g_fail_heap_call = 1;
        send_vm_response(1, NULL);
        if (g_last_status != 500 || g_json_calls != 0 || g_error_calls != 1 ||
            strcmp(g_error_code, "alloc_failed") != 0 ||
            g_response_alloc_count != 1)
            return fail("initial response allocation failure was not reported");
        printf("{\"status\":%u,\"code\":\"%s\"}",
               g_last_status, g_error_code);
        return 0;
    }

    if (strcmp(argv[1], "alloc-retry") == 0) {
        if (!fill_selector(600000)) return fail("selector did not fit the test VM");
        reset_capture();
        g_fail_heap_size = retry_capacity;
        send_vm_response(1, NULL);
        if (g_last_status != 500 || g_json_calls != 0 || g_error_calls != 1 ||
            strcmp(g_error_code, "alloc_failed") != 0 ||
            g_response_alloc_count != 2 ||
            g_response_alloc_sizes[0] != first_capacity ||
            g_response_alloc_sizes[1] != retry_capacity)
            return fail("retry allocation failure was not reported after growth");
        printf("{\"status\":%u,\"code\":\"%s\"}",
               g_last_status, g_error_code);
        return 0;
    }

    return fail("unknown scenario");
}
"""


def find_vcvars():
    if os.name != "nt":
        raise RuntimeError("this source-extraction harness requires Windows/MSVC")
    if not VSWHERE.is_file():
        raise RuntimeError("Visual Studio locator not found: %s" % VSWHERE)
    result = subprocess.run(
        [str(VSWHERE), "-latest", "-version", "[17.0,18.0)",
         "-products", "Microsoft.VisualStudio.Product.Community", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True, check=False, timeout=15)
    if result.returncode != 0 or not result.stdout.strip():
        raise RuntimeError("vswhere could not find the MSVC x64 tools")
    install = Path(result.stdout.splitlines()[0])
    vcvars = install / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.is_file():
        raise RuntimeError("MSVC x64 environment script not found: %s" % vcvars)
    return vcvars


def extract_production_builder():
    source = SOURCE.read_text(encoding="utf-8")
    begin = source.index("typedef struct {\n    char *buf;", source.index("The capacity-checked VM response builder"))
    end = source.index("static int build_host_info", begin)
    return source[begin:end]


def main():
    core_header = CORE_HEADER.read_text(encoding="utf-8")
    match = re.search(r"^\s*#define\s+ASB_MAX_VMS\s+(\d+)\b", core_header, re.MULTILINE)
    if not match:
        raise RuntimeError("ASB_MAX_VMS was not found in asb_core.h")
    preamble = PREAMBLE.replace("@ASB_MAX_VMS@", match.group(1))
    generated = preamble + "\n" + extract_production_builder() + "\n" + POSTAMBLE
    header_probe_source = (
        '#include "asb_net_limits.h"\n'
        'int main(void) { const wchar_t name[] = L"ok"; '
        'return asb_internal_switch_value_chars_ok(name, 2) ? 0 : 1; }\n')
    vcvars = find_vcvars()

    with tempfile.TemporaryDirectory(prefix="asb-response-builder-") as temp:
        temp_dir = Path(temp)
        c_file = temp_dir / "response_builder_harness.c"
        exe = temp_dir / "response_builder_harness.exe"
        header_probe_file = temp_dir / "net_limits_header_probe.c"
        header_probe_exe = temp_dir / "net_limits_header_probe.exe"
        cmd_file = temp_dir / "build_harness.cmd"
        c_file.write_text(generated, encoding="utf-8", newline="")
        header_probe_file.write_text(header_probe_source, encoding="ascii", newline="")
        vcvars_path = os.path.normpath(str(vcvars)).replace("/", "\\")
        exe_path = os.path.normpath(str(exe)).replace("/", "\\")
        source_path = os.path.normpath(str(c_file)).replace("/", "\\")
        header_include = os.path.normpath(str(ROOT / "src" / "backend_win")).replace("/", "\\")
        header_exe_path = os.path.normpath(str(header_probe_exe)).replace("/", "\\")
        header_source_path = os.path.normpath(str(header_probe_file)).replace("/", "\\")
        cmd_file.write_text(
            '@call "%s" >nul\r\n'
            '@if errorlevel 1 exit /b %%errorlevel%%\r\n'
            '@cl.exe /nologo /W4 /TC /I"%s" /Fe:"%s" "%s"\r\n'
            '@if errorlevel 1 exit /b %%errorlevel%%\r\n'
            '@cl.exe /nologo /W4 /TC /I"%s" /Fe:"%s" "%s"\r\n'
            % (vcvars_path, header_include, exe_path, source_path,
               header_include, header_exe_path, header_source_path),
            encoding="ascii", newline="")
        compiled = subprocess.run(["cmd.exe", "/d", "/c", cmd_file.name], cwd=temp_dir,
                                  capture_output=True, text=True, check=False, timeout=60)
        if compiled.returncode != 0:
            raise RuntimeError("MSVC harness build failed:\n%s%s"
                               % (compiled.stdout, compiled.stderr))
        probe = subprocess.run([str(header_probe_exe)], cwd=temp_dir,
                               capture_output=True, text=True, check=False, timeout=10)
        if probe.returncode != 0:
            raise RuntimeError("standalone asb_net_limits.h probe failed:\n%s%s"
                               % (probe.stdout, probe.stderr))
        print("PASS asb_net_limits.h standalone include")

        for scenario, expected_code in (
                ("too-large", "too_large"),
                ("alloc-first", "alloc_failed"),
                ("alloc-retry", "alloc_failed")):
            result = subprocess.run([str(exe), scenario], cwd=temp_dir,
                                    capture_output=True, text=True, check=False,
                                    timeout=30)
            if result.returncode != 0:
                raise RuntimeError("%s scenario failed:\n%s%s"
                                   % (scenario, result.stdout, result.stderr))
            response = json.loads(result.stdout)
            if response != {"status": 500, "code": expected_code}:
                raise RuntimeError("%s returned unexpected error: %r"
                                   % (scenario, response))
            print("PASS %s -> HTTP %d %s" %
                  (scenario, response["status"], response["code"]))

        grown = subprocess.run([str(exe), "grow"], cwd=temp_dir,
                               capture_output=True, check=False, timeout=30)
        if grown.returncode != 0:
            raise RuntimeError("grow scenario failed:\n%s%s"
                               % (grown.stdout.decode("utf-8", errors="replace"),
                                  grown.stderr.decode("utf-8", errors="replace")))
        body = json.loads(grown.stdout.decode("utf-8"))
        selector = body["vms"][0]["internalSwitch"]
        if len(selector) != 600000 or set(selector) != {"x"}:
            raise RuntimeError("grown response did not preserve all 600000 selector characters")
        print("PASS grow -> strict JSON; exact 600000-character selector; 512 KiB -> 1 MiB")

        single_grown = subprocess.run([str(exe), "single-grow"], cwd=temp_dir,
                                      capture_output=True, check=False, timeout=30)
        if single_grown.returncode != 0:
            raise RuntimeError("single-grow scenario failed:\n%s%s"
                               % (single_grown.stdout.decode("utf-8", errors="replace"),
                                  single_grown.stderr.decode("utf-8", errors="replace")))
        single = json.loads(single_grown.stdout.decode("utf-8"))
        selector = single.get("internalSwitch") if isinstance(single, dict) else None
        if not isinstance(single, dict) or single.get("name") != "harness-vm" or \
                not isinstance(selector, str) or len(selector) != 600000 or \
                set(selector) != {"x"}:
            raise RuntimeError("single-object growth did not preserve the exact selector")
        print("PASS single-grow -> strict JSON object; exact 600000-character selector; shared edit path")


if __name__ == "__main__":
    main()
