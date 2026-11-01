"""
No-VM checks (run once by the harness): (1) host cores/RAM cross-checked against
the exact OS calls the daemon makes (Win32 on Windows, sysctl on macOS);
(2) create-input-validation rejections -- all rejected before any VM is built,
matching the GUI's JS guards for THIS host platform; (3) capability gates:
snapshots/templates are advertised in version()["capabilities"] and their
routes return 501 where unsupported. The per-VM edit-validation +
duplicate-name paths live in vm_lifecycle.py.
"""
import sys, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # for asb.py (parent dir)
import asb

IS_MAC = sys.platform == "darwin"

c = asb.connect()
_fails = []
def check(label, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + label + (("   " + detail) if detail else ""), flush=True)
    if not cond:
        _fails.append(label + (("  (" + detail + ")") if detail else ""))
def msg(body):
    return (body.get("error", {}) or {}).get("message", "")


if IS_MAC:
    def os_cores():
        return int(subprocess.check_output(["sysctl", "-n", "hw.physicalcpu"]).strip())
    def os_ram_mb():
        return int(subprocess.check_output(["sysctl", "-n", "hw.memsize"]).strip()) // (1024 * 1024)
else:
    import ctypes

    class SYSTEM_INFO(ctypes.Structure):
        _fields_ = [("wProcessorArchitecture", ctypes.c_ushort), ("wReserved", ctypes.c_ushort),
                    ("dwPageSize", ctypes.c_ulong), ("lpMinimumApplicationAddress", ctypes.c_void_p),
                    ("lpMaximumApplicationAddress", ctypes.c_void_p), ("dwActiveProcessorMask", ctypes.c_void_p),
                    ("dwNumberOfProcessors", ctypes.c_ulong), ("dwProcessorType", ctypes.c_ulong),
                    ("dwAllocationGranularity", ctypes.c_ulong), ("wProcessorLevel", ctypes.c_ushort),
                    ("wProcessorRevision", ctypes.c_ushort)]
    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    def os_cores():
        si = SYSTEM_INFO(); ctypes.windll.kernel32.GetSystemInfo(ctypes.byref(si)); return int(si.dwNumberOfProcessors)
    def os_ram_mb():
        m = MEMORYSTATUSEX(); m.dwLength = ctypes.sizeof(m)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)); return int(m.ullTotalPhys // (1024 * 1024))


print("=== host info vs the real machine ===")
host = c.host()
check("hostCores matches the OS", host["hostCores"] == os_cores(), "daemon=%s os=%s" % (host["hostCores"], os_cores()))
check("hostRamMb matches the OS", host["hostRamMb"] == os_ram_mb(), "daemon=%s os=%s" % (host["hostRamMb"], os_ram_mb()))
check("vmCores / vmRamMb present and >= 0", host.get("vmCores", -1) >= 0 and host.get("vmRamMb", -1) >= 0)
if not IS_MAC:
    check("Windows GPU records expose boolean isNvidia",
          all(isinstance(g.get("isNvidia"), bool) for g in host.get("gpus", [])),
          "gpus=%r" % host.get("gpus", []))
v = c.version()
check("version product == AppSandbox", v.get("product") == "AppSandbox")
caps = v.get("capabilities", {})
check("capabilities advertised", isinstance(caps.get("snapshots"), bool) and isinstance(caps.get("templates"), bool), "%s" % caps)
if caps.get("templates"):
    check("templates is a list", isinstance(c.templates(), list))
else:
    check("templates route -> 501", c._req("GET", "/templates")[0] == 501)

print("\n=== create-input validation (rejected before any build) ===")
if IS_MAC:
    # A macOS host supports macOS AND Windows guests (Linux is HCS-only); a
    # Windows guest needs an ISO (imagePath), while a macOS guest's image is
    # OPTIONAL (the daemon auto-fetches the restore IPSW). macOS name limit is
    # 63 (LocalHostName); username follows the Windows-account ruleset.
    MAC = dict(osType="macOS", adminUser="user", adminPass="test123")
    CASES = [
        ("missing name",              dict(MAC, name=""),                          "name is required"),
        ("name > 63 chars",           dict(MAC, name="a" * 64),                    "63 characters"),
        ("illegal name chars",        dict(MAC, name="bad name"),                  "letters, digits"),
        ("name all digits",           dict(MAC, name="12345"),                     "only digits"),
        ("name leading hyphen",       dict(MAC, name="-bad"),                      "hyphen"),
        ("Windows on macOS needs ISO",dict(MAC, name="okname", osType="Windows"),  "ISO"),
        ("Linux guest on macOS",      dict(MAC, name="okname", osType="Linux"),    "macOS host"),
        ("template (win-only)",       dict(MAC, name="okname", isTemplate=True),   "Windows"),
        ("reserved username",         dict(MAC, name="okname", adminUser="CON"),   "reserved"),
        ("username > 20",             dict(MAC, name="okname", adminUser="a" * 21), "20 characters"),
        ("username only dots",        dict(MAC, name="okname", adminUser="..."),   "dots or spaces"),
        ("odd RAM",                   dict(MAC, name="okname", ramMb=4001),        "2 MB-aligned"),
        ("RAM < 512",                 dict(MAC, name="okname", ramMb=256),         "at least 512"),
        ("gpuMode out of range",      dict(MAC, name="okname", gpuMode=9),         "gpuMode"),
        ("networkMode out of range",  dict(MAC, name="okname", networkMode=9),     "networkMode"),
        ("sshDeployKey w/o ssh",      dict(MAC, name="okname", sshDeployKey=True), "requires sshEnabled"),
    ]
else:
    WIN = dict(osType="Windows", imagePath="x.iso", adminUser="user", adminPass="test123")
    LIN = dict(osType="Linux", imagePath="x.iso", adminUser="user", adminPass="test123")
    CASES = [
        ("missing name",              dict(WIN, name=""),                          "name is required"),
        ("Windows name > 15 chars",   dict(WIN, name="abcdefghijklmnop"),          "15 characters"),
        ("Linux name uppercase",      dict(LIN, name="UpperName"),                 "lowercase"),
        ("illegal name chars",        dict(WIN, name="bad name"),                  "letters, digits"),
        ("name all digits",           dict(WIN, name="12345"),                     "only digits"),
        ("name leading hyphen",       dict(WIN, name="-bad"),                      "hyphen"),
        ("no image and no template",  dict(osType="Windows", name="okname", adminUser="user", adminPass="p"), "image"),
        ("Linux template (win-only)", dict(LIN, name="oklin", isTemplate=True),    "Windows"),
        ("Linux bad username",        dict(LIN, name="oklin", adminUser="Bad User"), "username"),
        ("Windows reserved username", dict(WIN, name="okwin", adminUser="CON"),    "reserved"),
        ("Windows username > 20",     dict(WIN, name="okwin", adminUser="a" * 21),  "20 characters"),
        ("Linux empty password",      dict(LIN, name="oklin", adminUser="user", adminPass=""), "Password is required"),
        ("odd RAM",                   dict(WIN, name="okwin", ramMb=4001),         "2 MB-aligned"),
        ("RAM < 512",                 dict(WIN, name="okwin", ramMb=256),          "at least 512"),
        ("gpuMode out of range",      dict(WIN, name="okwin", gpuMode=9),          "gpuMode"),
        ("networkMode out of range",  dict(WIN, name="okwin", networkMode=9),      "networkMode"),
        ("sshDeployKey w/o ssh",      dict(WIN, name="okwin", sshDeployKey=True),  "requires sshEnabled"),
    ]

before = {v["name"] for v in c.list()}
for label, cfg, sub in CASES:
    code, body = c._req("POST", "/vms", cfg)
    check("reject: %s" % label, code == 400 and sub.lower() in msg(body).lower(), "code=%s msg=%r" % (code, msg(body)))
check("no VM created by any invalid request", {v["name"] for v in c.list()} == before)

print("\n" + ("==== STATIC: ALL PASS ====" if not _fails else "==== %d FAIL ====\n  - %s" % (len(_fails), "\n  - ".join(_fails))))
sys.exit(1 if _fails else 0)
