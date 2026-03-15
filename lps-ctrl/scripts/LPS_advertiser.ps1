<#
.SYNOPSIS
ESP32 BLE LPS Controller CLI

.DESCRIPTION
Broadcasts BLE commands via PowerShell. Uses WinRT API to send Service Data (0x16) advertisements.
Bypasses PowerShell 5.1 type-casting issues by invoking the .NET CLR directly for WinRT collections.

.EXAMPLE
.\LPS_advertiser.ps1 -CmdType 1 -TargetId -1 (or .\LPS_advertiser.ps1 -c 1 -t -1)
.\LPS_advertiser.ps1 -CmdType 5 -TargetId 2 -R 255 -G 0 -B 0
.\LPS_advertiser.ps1 -CmdType 6 -CancelId 3
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory=$true, HelpMessage="CMD (1:PLAY, 2:PAUSE, 3:STOP, 4:RELEASE, 5:TEST, 6:CANCEL, 7:CHECK, 8:UPLOAD, 9:RESET)")]
    [Alias('c')]
    [ValidateRange(1,9)]
    [int]$CmdType,

    [Parameter(Mandatory=$true, HelpMessage="Target ID (0-63). Use -1 for global broadcast.")]
    [Alias('t')]
    [ValidateRange(-1,63)]
    [int]$TargetId,

    [Parameter(Mandatory=$false, HelpMessage="Command ID (0-15)")]
    [ValidateRange(0,15)]
    [int]$CmdId = 0,

    [Parameter(Mandatory=$false)]
    [int]$DelayMs = 2000,

    [Parameter(Mandatory=$false)]
    [int]$PrepMs = 1000,

    [Parameter(Mandatory=$false)]
    [byte]$R = 0,

    [Parameter(Mandatory=$false)]
    [byte]$G = 0,

    [Parameter(Mandatory=$false)]
    [byte]$B = 0,

    [Parameter(Mandatory=$false)]
    [int]$CancelId = 0
)

# --- 1. Payload Assembly (Update UUID as needed) ---
$uuid1 = [byte]0x00
$uuid2 = [byte]0x00

# Combine CmdId (High 4 bits) and CmdType (Low 4 bits)
$cmdInfo = [byte]((($CmdId -band 0x0F) -shl 4) -bor ($CmdType -band 0x0F))

# Generate 8-byte Target Mask
$maskBytes = New-Object byte[] 8
if ($TargetId -eq -1) {
    # Set all bits to 1 for global broadcast
    for ($i = 0; $i -lt 8; $i++) { $maskBytes[$i] = 0xFF }
} else {
    # Shift bit 1 to the target ID position
    $mask = [uint64]1 -shl $TargetId
    $maskBytes = [BitConverter]::GetBytes([uint64]$mask)
    if (-not [BitConverter]::IsLittleEndian) { [Array]::Reverse($maskBytes) }
}

# Convert Delay to 4-byte Big-Endian
$delayBytes = [BitConverter]::GetBytes([uint32]$DelayMs)
if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($delayBytes) }

# Handle Command-Specific Payload (4 bytes)
$specBytes = New-Object byte[] 4
if ($CmdType -eq 1) { 
    # PLAY: Prep LED time
    $prepBytes = [BitConverter]::GetBytes([uint32]$PrepMs)
    if ([BitConverter]::IsLittleEndian) { [Array]::Reverse($prepBytes) }
    $specBytes = $prepBytes
} elseif ($CmdType -eq 5) { 
    # TEST: RGB colors
    $specBytes[0] = $R
    $specBytes[1] = $G
    $specBytes[2] = $B
} elseif ($CmdType -eq 6) { 
    # CANCEL: Slot ID
    $specBytes[0] = [byte]($CancelId -band 0x0F)
}

# Construct Final Byte Array
$payloadList = [System.Collections.Generic.List[byte]]::new()
$payloadList.Add($uuid1)
$payloadList.Add($uuid2)
$payloadList.Add($cmdInfo)
$payloadList.AddRange($maskBytes)
$payloadList.AddRange($delayBytes)
$payloadList.AddRange($specBytes)

$payload = $payloadList.ToArray()
$hexString = [BitConverter]::ToString($payload) -replace '-'
Write-Host "Assembled Payload (Hex): $hexString" -ForegroundColor Cyan


# --- 2. WinRT BLE Advertising ---

# Define required Windows Metadata assemblies
$assemblies = @(
    "System.Runtime.WindowsRuntime",
    "$env:windir\System32\WinMetadata\Windows.Foundation.winmd",
    "$env:windir\System32\WinMetadata\Windows.Devices.winmd"
)

try {
    # Load WindowsRuntime assembly for IBuffer support
    Add-Type -AssemblyName System.Runtime.WindowsRuntime

    # Resolve WinRT types
    $advPublisherType = [Type]::GetType("Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisher, Windows.Devices.Bluetooth, ContentType=WindowsRuntime")
    $advDataSectionType = [Type]::GetType("Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementDataSection, Windows.Devices.Bluetooth, ContentType=WindowsRuntime")

    # Wrap the payload into a WinRT IBuffer
    $buffer = [System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBuffer]::Create($payload, 0, $payload.Length, $payload.Length)

    # Instantiate Publisher and Data Section (Service Data 0x16)
    $publisher = [Activator]::CreateInstance($advPublisherType)
    $section = [Activator]::CreateInstance($advDataSectionType)
    $section.DataType = 0x16
    $section.Data = $buffer

    # 1. Create a generic ICollection<BluetoothLEAdvertisementDataSection> interface
    $collectionType = [System.Collections.Generic.ICollection`1].MakeGenericType($advDataSectionType)
    
    # 2. Extract the 'Add' method from the interface
    $addMethod = $collectionType.GetMethod("Add")
    
    # 3. Use .NET Reflection to invoke 'Add' (Avoids PowerShell's WinRT adapter errors)
    $addMethod.Invoke($publisher.Advertisement.DataSections, [object[]]@($section))

    # Start broadcasting
    Write-Host "Broadcasting BLE Advertisement..." -ForegroundColor Green
    $publisher.Start()
    
    # Keep the broadcast active for 1 second
    Start-Sleep -Seconds 1
    
    $publisher.Stop()
    Write-Host "Broadcast complete." -ForegroundColor Yellow

} catch {
    Write-Error "BLE Advertisement failed: $_"
}