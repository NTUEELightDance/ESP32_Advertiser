# ESP32 BLE LPS Controller CLI

This is a PowerShell-based command-line tool used to broadcast commands to the ESP32 LPS controller via BLE (Bluetooth Low Energy).

This script sends Bluetooth Service Data (`0x16`) advertisement packets by calling the Windows WinRT API, and utilizes .NET Reflection to directly invoke the CLR, bypassing the common type-casting errors in PowerShell 5.1 when handling WinRT collection types.

## System Requirements

* **Operating System**: Windows 10 or Windows 11 (must support WinRT API).
* **Hardware**: Bluetooth-supported network card or receiver (please ensure Windows Bluetooth is turned on before execution).
* **Environment**: PowerShell 5.1 or higher (running as Administrator is recommended to ensure Bluetooth broadcasting permissions).

## Parameter Description

When executing the script, you can customize the broadcast commands using the following parameters:

| Parameter | Alias | Type | Default Value | Description |
| --- | --- | --- | --- | --- |
| `-CmdType` | `-c` | Int | **(Required)** | Command type. Valid values: `1`:PLAY, `2`:PAUSE, `3`:STOP, `4`:RELEASE, `5`:TEST, `6`:CANCEL, `7`:CHECK, `8`:UPLOAD, `9`:RESET |
| `-TargetIds` | `-t` | String | **(Required)** | Target device IDs. Supports multi-target via comma-separated string (e.g., `"1,2,5"`). Use `"all"` or `"-1"` for Global Broadcast. |
| `-CmdId` |  | Int | `0` | Command ID (`0-15`). |
| `-DelayMs` |  | Int | `2000` | Delay time in milliseconds (ms). |
| `-PrepMs` |  | Int | `1000` | Preparation time in milliseconds (ms). **Only applicable for the `PLAY` (Type 1) command.** |
| `-R` |  | Byte | `0` | Red color value (`0-255`). **Only applicable for the `TEST` (Type 5) command.** |
| `-G` |  | Byte | `0` | Green color value (`0-255`). **Only applicable for the `TEST` (Type 5) command.** |
| `-B` |  | Byte | `0` | Blue color value (`0-255`). **Only applicable for the `TEST` (Type 5) command.** |
| `-CancelId` |  | Int | `0` | Slot ID to cancel. **Only applicable for the `CANCEL` (Type 6) command.** |

## Usage Examples

**1. Global Play (PLAY)**
Send a PLAY command to all devices using the default delay and preparation time:

```powershell
.\LPS_advertiser.ps1 -CmdType 1 -TargetIds "all"
```

**2. Multi-Device Test (TEST)**
Send a TEST command to devices 1, 3, and 5, and light up the red LED (R:255, G:0, B:0):

```powershell
.\LPS_advertiser.ps1 -CmdType 5 -TargetIds "1,3,5" -R 255 -G 0 -B 0
```

**3. Cancel Specific Schedule (CANCEL)**
Send a CANCEL command, specifying to cancel the task with Slot ID 3:

```powershell
.\LPS_advertiser.ps1 -CmdType 6 -TargetIds "all" -CancelId 3
```

## Payload Structure

Under the hood, the script assembles the parameters into a 19-byte Service Data packet with the following structure:

* **Byte 0-1**: Magic Bytes (`0x4C`, `0x44` representing "LD").
* **Byte 2**: Command Info (combined from the high 4 bits of `CmdId` and the low 4 bits of `CmdType`).
* **Byte 3-10**: Target Mask (8-byte mask dynamically generated from the `-TargetIds` string).
* **Byte 11-14**: Delay time (4 Bytes, Big-Endian).
* **Byte 15-18**: Specific command payload (4 Bytes, dynamically determined by `CmdType`):
* `Type 1 (PLAY)`: Passes `PrepMs` (Big-Endian)
* `Type 5 (TEST)`: Passes RGB values (`R`, `G`, `B`)
* `Type 6 (CANCEL)`: Passes `CancelId`



## Troubleshooting

* **Broadcast failure or exception error**: Please confirm that Windows Bluetooth is turned on. If an access denied issue occurs, try opening the PowerShell window as an "Administrator" before running the script.
* **ESP32 not receiving signals**: Please check if the UUID parsing on the ESP32 side matches the `0x4C` and `0x44` (Magic bytes) declared at the beginning of the script.
