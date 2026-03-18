# Check which GPUs support GPU-PV partitioning
Write-Output "=== Partitionable GPUs (Hyper-V WMI) ==="
Get-CimInstance -Namespace root/virtualization/v2 -ClassName Msvm_PartitionableGpu -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Output ("  Name: {0}" -f $_.Name)
}

Write-Output "`n=== Video Controllers ==="
Get-CimInstance Win32_VideoController | ForEach-Object {
    Write-Output ("  {0} | PNP: {1} | Status: {2}" -f $_.Name, $_.PNPDeviceID, $_.Status)
}

# EnumDisplayDevices to check PRIMARY_DEVICE flag
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

Write-Output "`n=== EnumDisplayDevices ==="
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
    }
}
