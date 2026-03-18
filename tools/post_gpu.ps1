# Method 1: EnumDisplayDevices — PRIMARY_DEVICE flag
Add-Type @"
using System;
using System.Runtime.InteropServices;

public class DisplayDeviceHelper {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DISPLAY_DEVICE {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);
}
"@

Write-Output "=== EnumDisplayDevices ==="
for ($i = 0; $i -lt 16; $i++) {
    $dd = New-Object DisplayDeviceHelper+DISPLAY_DEVICE
    $dd.cb = [Runtime.InteropServices.Marshal]::SizeOf($dd)
    if ([DisplayDeviceHelper]::EnumDisplayDevices($null, $i, [ref]$dd, 0)) {
        $flags = @()
        if ($dd.StateFlags -band 0x00000001) { $flags += "ACTIVE" }
        if ($dd.StateFlags -band 0x00000004) { $flags += "PRIMARY_DEVICE" }
        if ($dd.StateFlags -band 0x00000008) { $flags += "MIRRORING_DRIVER" }
        Write-Output ("  [{0}] {1} | {2} | Flags: {3} (0x{4:X8})" -f $i, $dd.DeviceName, $dd.DeviceString, ($flags -join ","), $dd.StateFlags)
        Write-Output ("       DeviceID: {0}" -f $dd.DeviceID)
        Write-Output ("       DeviceKey: {0}" -f $dd.DeviceKey)
    }
}

# Method 2: DXGI adapter order
Write-Output "`n=== DXGI Adapters (via PowerShell) ==="
Get-CimInstance Win32_VideoController | Select-Object Name, PNPDeviceID, AdapterCompatibility, VideoProcessor | Format-List

# Method 3: Check registry for POST device
Write-Output "=== Registry: Control\GraphicsDrivers ==="
$postDev = Get-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" -ErrorAction SilentlyContinue
if ($postDev) {
    $postDev | Format-List
}

# Method 4: Check Video registry keys
Write-Output "=== Registry: Control\Video ==="
$videoKeys = Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Control\Video" -ErrorAction SilentlyContinue
foreach ($vk in $videoKeys) {
    $sub0 = Join-Path $vk.PSPath "0000"
    if (Test-Path $sub0) {
        $props = Get-ItemProperty $sub0 -ErrorAction SilentlyContinue
        if ($props.DriverDesc) {
            Write-Output ("  {0}: {1}" -f $vk.PSChildName, $props.DriverDesc)
            if ($props.Device) { Write-Output ("    Device: {0}" -f $props.Device) }
        }
    }
}
