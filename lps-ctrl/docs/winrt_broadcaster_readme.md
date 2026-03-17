# ESP32 BLE WinRT Broadcaster (`pc_adv_ex.py`)

This is an interactive Python command-line tool designed to broadcast BLE commands to ESP32 receivers directly from a Windows PC. 

By utilizing native Windows WinRT APIs (`BluetoothLEAdvertisementPublisher`), this script acts as a **Pure Broadcaster**. It bypasses standard GATT Server overhead, allowing the custom 19-byte command payload to be embedded directly into the primary advertisement packet (`ADV_IND`) using the Service Data (`0x16`) format. This ensures zero-latency synchronization for receiver nodes operating in Passive Scanning mode.

## System Requirements

* **Operating System**: Windows 10 or Windows 11 (Requires WinRT API support).
* **Hardware**: Built-in Bluetooth adapter or USB Bluetooth dongle.
* **Python**: Python 3.10 is highly recommended to ensure maximum compatibility with the C++ bindings of the `winrt` library.

## Installation

This script requires specific Windows Runtime bindings. Ensure your `pyproject.toml` is configured with the following dependencies:

```toml
dependencies = [
    "winrt-Windows.Foundation",
    "winrt-Windows.Foundation.Collections",
    "winrt-Windows.Devices.Bluetooth",
    "winrt-Windows.Devices.Bluetooth.Advertisement",
    "winrt-Windows.Storage.Streams"
]
```

It is recommended to create a virtual environment in the `lps-ctrl` directory (where `pyproject.toml` is located) and install the required packages.

```bash
pip install -e .
```

## Usage (Interactive Mode)

Start the interactive terminal by running the script:

```bash
python examples/pc_adv_ex.py
```

The script will guide you through a step-by-step prompt to assemble and broadcast your commands:

**Step 1: Command Code**
Enter a number from `1` to `9` to select the action (e.g., `1` for PLAY, `5` for TEST).

**Step 2: Target IDs (Multi-select supported)**
You can control specific players or all of them at once:

* Type `all` to broadcast to the entire network.
* Type a single ID (e.g., `2`) to target Player 2.
* Type comma-separated IDs (e.g., `1, 3, 5`) to target Player 1, 3, and 5 simultaneously.

**Step 3: Timing Controls**

* **Delay Time (ms)**: Set how long the ESP32 should wait before executing the command (Default is `2000`ms). Just press `Enter` to use the default.
* **Prep Time (ms)**: *(Only for PLAY commands)* Set how long the red preparation LED should light up before playback starts (Default is `1000`ms).

**Step 4: Special Parameters**

* **For TEST (`5`)**: Enter RGB values like `255,0,0` for Red. Press `Enter` to use the default breathing light pattern.
* **For CANCEL (`6`)**: Enter the specific `CMD_ID` (0-15) you want to abort.

### 📡 Active Listener Mode (CHECK Command)

When issuing the **CHECK** (`7`) command, the script automatically switches to an Active Listener mode (`BluetoothLEAdvertisementWatcher`). It will temporarily stop broadcasting, listen for incoming ACK packets (Manufacturer Data containing `0xFFFF` and `0x07`) from the receivers, and print out their current states and remaining delay times directly in the terminal.

### Available Commands

| Code | Command | Description |
| --- | --- | --- |
| `1` | **PLAY** | Start timeline/playback. |
| `2` | **PAUSE** | Pause playback. |
| `3` | **STOP** | Stop and reset position. |
| `4` | **RELEASE** | Release memory/Unload. |
| `5` | **TEST** | LED Color Test Mode. |
| `6` | **CANCEL** | Cancel a specific pending command. |
| `7` | **CHECK** | Request status report. Triggers Listener Mode. |
| `8` | **UPLOAD** | Trigger OTA update sequence. |
| `9` | **RESET** | System Reboot. |

## Payload Structure (Service Data `0x16`)

The script dynamically generates a 19-byte payload appended to the `0x16` Service Data section:

* **Byte 0-1**: Magic Bytes (`0x4C`, `0x44` representing "LD").
* **Byte 2**: Command Info (High 4 bits = Command ID sequence, Low 4 bits = Command Type).
* **Byte 3-10**: Target Mask (8 Bytes, Little-Endian bitmask dynamically generated).
* **Byte 11-14**: Delay Time (4 Bytes, Big-Endian in milliseconds).
* **Byte 15-18**: Specific Parameters (4 Bytes):
* `PLAY`: Preparation time in milliseconds.
* `TEST`: RGB values (`R`, `G`, `B`).
* `CANCEL`: Target Command ID to cancel.



## Troubleshooting

* **Script crashes immediately**: Ensure you are running Python 3.10 and all the `winrt` dependencies are properly installed (including `Foundation` and `Collections`).
* **ESP32 does not respond**: Verify that your ESP32 receiver firmware is configured to scan for Service Data (`0x16`) and that its target UUID check matches `0x4C` and `0x44`. Also, ensure the ESP32 is using Passive Scanning (`0x00`) for optimal reception.
* **CHECK command yields no reports**: Ensure your PC's Bluetooth adapter is fully active and not blocked by background Windows services. The ESP32 must be properly configured to broadcast its ACK packet back to the host.