#!/usr/bin/env python3
"""Check the bundled engine's version and both ivshmem PCI device IDs under HVF.

Creates one paused VM with 256 MiB RAM, one CPU and temporary shared memory.
No guest disk, network interface or existing AppSandbox VM is used.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=root / "dist/bin/qemu-system-aarch64")
    options = parser.parse_args()
    expected = json.loads((root / "source-version.json").read_text())["upstreamTag"].lstrip("v")
    with tempfile.TemporaryDirectory(prefix="asb-qemu-check-") as directory:
        args = [str(options.qemu), "-machine", "virt,gic-version=3,highmem=on",
                "-accel", "hvf", "-cpu", "host", "-m", "256", "-smp", "1",
                "-nodefaults", "-display", "none", "-serial", "none",
                "-monitor", "none", "-qmp", "stdio", "-S", "-nic", "none"]
        for number, device_id in enumerate((None, 0x1111)):
            path = Path(directory) / f"memory{number}"
            with path.open("wb") as stream:
                stream.truncate(1024 * 1024)
            args += ["-object", f"memory-backend-file,id=mem{number},size=1M,mem-path={path},share=on",
                     "-device", f"ivshmem-plain,memdev=mem{number}" +
                     (f",x-device-id={device_id}" if device_id is not None else "")]
        commands = [dict(execute=command, id=command) for command in
                    ("qmp_capabilities", "query-version", "query-pci", "quit")]
        result = subprocess.run(args, input="".join(json.dumps(c) + "\n" for c in commands),
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"QEMU exited {result.returncode}: {result.stderr}")
        replies = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        errors = [reply for reply in replies if "error" in reply]
        if errors:
            raise RuntimeError(f"QMP errors: {errors}")
        by_id = {reply["id"]: reply["return"] for reply in replies if "return" in reply and "id" in reply}
        version = by_id["query-version"]["qemu"]
        actual = ".".join(str(version[key]) for key in ("major", "minor", "micro"))
        assert actual == expected, (actual, expected)

        def devices(buses):
            for bus in buses:
                for device in bus["devices"]:
                    yield device
                    if "pci_bridge" in device:
                        yield from devices([device["pci_bridge"]["bus"]])

        ids = {(device["id"]["vendor"], device["id"]["device"])
               for device in devices(by_id["query-pci"])}
        assert {(0x1AF4, 0x1110), (0x1AF4, 0x1111)} <= ids, ids
        print(f"PASS: QEMU {actual}, HVF, and concurrent ivshmem devices 1af4:1110 / 1af4:1111")


if __name__ == "__main__":
    main()
