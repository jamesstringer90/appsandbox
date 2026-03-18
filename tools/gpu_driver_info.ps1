$devs = Get-PnpDevice -Class Display
foreach ($d in $devs) {
    $id = $d.InstanceId
    $enumPath = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
    $drv = (Get-ItemProperty -Path $enumPath -Name Driver -ErrorAction SilentlyContinue).Driver
    if ($drv) {
        $classPath = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drv"
        $inf = (Get-ItemProperty -Path $classPath -Name InfPath -ErrorAction SilentlyContinue).InfPath
        Write-Output "$($d.FriendlyName) | InstanceId=$id | InfPath=$inf"

        # The oem*.inf maps to the original INF in the DriverStore.
        # Read the actual oem inf file to find the DriverStore path reference
        $infFullPath = "$env:SystemRoot\INF\$inf"
        if (Test-Path $infFullPath) {
            # Use pnputil to find the driver store location
            $pnpInfo = pnputil /enum-drivers | Select-String -Pattern $inf -Context 0,10
            Write-Output "  pnputil info: $pnpInfo"
        }

        # Also try: look at DriverStoreLocation via Get-PnpDeviceProperty
        $dslProp = Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue
        Write-Output "  DEVPKEY_Device_DriverInfPath: $($dslProp.Data)"

        # Try the published driver store approach
        $storeProp = Get-PnpDeviceProperty -InstanceId $id -KeyName '{a8b865dd-2e3d-4094-ad97-e593a70c75d6} 5' -ErrorAction SilentlyContinue
        if ($storeProp) {
            Write-Output "  DriverStoreInfPath: $($storeProp.Data)"
        }
    }
}

# Also just list all GPU-related looking folders in FileRepository
Write-Output "`n--- FileRepository GPU folders ---"
$repoPath = "$env:SystemRoot\System32\DriverStore\FileRepository"
Get-ChildItem $repoPath -Directory | Where-Object { $_.Name -match 'nv_|nvd|igdlh|iigd|amd|ati' } | ForEach-Object { Write-Output $_.Name }
