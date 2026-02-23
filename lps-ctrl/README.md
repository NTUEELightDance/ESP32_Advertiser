# ESP32 BLE Sender - UART Controlled

This project provides a Python module, `lps_ctrl.py`, which implements a system to control an ESP32 via UART (USB Serial) from a PC. The ESP32 acts as a central **Sender** to broadcast Bluetooth Low Energy (BLE) command packets using a non-blocking scheduler. This allows for precise, interleaved broadcasting of multiple commands to distributed receivers (e.g., light suits).

## Installation

It is recommended to create a virtual environment in the `lps-ctrl` directory (where `pyproject.toml` is located) and install the required packages.

```bash
python3 -m venv venv
source venv/bin/activate
pip install -e .
```

## System Workflow

```mermaid
sequenceDiagram
    participant User as Python Script
    participant Lib as lps_ctrl.py
    participant USB as USB/UART
    participant ESP as ESP32 (Sender)
    participant RX as Receiver Devices

    Note over User, ESP: 1. Command Scheduling Phase
    User->>Lib: send_burst(cmd='PLAY', delay=2.0s)
    Lib->>USB: Send "1,2000000,0,0,0,0,0\n"
    USB->>ESP: RX Interrupt & Parse
    ESP->>ESP: Add to Scheduler (Slot X) & Set Target Time
    ESP-->>Lib: ACK:OK
    Lib-->>User: Return JSON {statusCode: 0}

    Note over ESP, RX: 2. Continuous Broadcasting Phase (Countdown)
    loop Until Target Time is Reached
        ESP->>RX: Broadcast Packet (Remaining: 1.9s)
        RX->>RX: Sync Clock based on Remaining Time
        ESP->>RX: Broadcast Packet (Remaining: 1.8s)
        ESP->>RX: Broadcast Packet (Remaining: ...s)
    end
    
    Note over RX: 3. Execution Phase
    RX->>RX: Execute 'PLAY' exactly at Target Time

```

### Key Concepts

1. **PC-Side (lps_ctrl)**: Formats parameters into a CSV string and sends it via Serial. It waits for an `ACK` from the ESP32 to confirm the command was accepted.
2. **ESP32 Scheduler (Immediate Broadcast)**:
* Once a task is added, the ESP32 **immediately starts broadcasting** it in a round-robin fashion.
* The broadcast packet contains the **remaining time** (counting down in real-time) until the target execution timestamp.
* Broadcasting stops automatically when the target timestamp is reached.


3. **Synchronization**: Receivers listen for these packets. Even if they receive the packet at different times (e.g., one at 1.9s remaining, another at 0.5s remaining), they both calculate the same absolute **Target Execution Time**, ensuring synchronized action.

## TCP File Sender (Wi-Fi Update)

In addition to the BLE command broadcasting, the system features a dedicated TCP Server module to distribute timeline files (`control.dat` and `frame.dat`) to up to 32 individual receivers over a local Wi-Fi network. 

### TCP System Workflow

```mermaid
sequenceDiagram
    participant PC as Python TCP Server
    participant ESP as ESP32 (Receiver)
    participant SD as SD Card

    Note over PC, ESP: 1. Network Setup Phase
    PC->>PC: Start TCP Server (Listening on Port 3333)
    ESP->>ESP: Stop BLE & Initialize Wi-Fi
    ESP->>PC: Connect to TCP Server
    
    Note over PC, ESP: 2. Identification & Validation
    ESP->>PC: Send Player ID
    PC->>PC: Verify if Player 1's files exist
    
    Note over PC, SD: 3. Download Phase
    PC->>ESP: Send File Size (4 bytes)
    PC->>ESP: Send control.dat Data Chunks
    ESP->>SD: Write to SD Card
    PC->>ESP: Send frame.dat Data Chunks
    ESP->>SD: Write to SD Card
    
    Note over PC, ESP: 4. Completion Phase
    ESP->>PC: Send ACK ("DONE\n")
    PC->>PC: Close Connection for Player 1
    ESP->>ESP: Stop Wi-Fi & Re-init BLE Receiver
```
### Key Features
1. **Dynamic Path Routing**: The server automatically routes requests to specific directories based on the received Player ID (Supporting Player 1 to 32).

2. **Auto-Recovery**: After a successful download and sending the DONE ACK, the ESP32 automatically shuts down its Wi-Fi modem and restarts the BLE scanning task to return to the performance state.

## API Documentation

### Class: `ESP32BTSender`

```python
__init__(port, baud_rate=115200, timeout=1)
```

* **port** (Required): Serial port name (e.g., `'COM3'` on Windows or `'/dev/ttyS3'` on Linux).
* **baud_rate**: Default is `115200`. Must match the `main.c` setting in the firmware.
* **timeout**: Default is `1` second.

### Method: `send_burst`

Sends a command packet to the ESP32.

```python
send_burst(cmd_input, delay_sec, prep_led_sec, target_ids, data)
```

#### Parameters

| Parameter | Type | Description |
| --- | --- | --- |
| **cmd_input** | `str` | Command type (see Mapping Table below). |
| **delay_sec** | `float` | Time in seconds before the command executes. **Must be > 1.0s**. |
| **prep_led_sec** | `float` | Duration for the "Preparation LED" effect. |
| **target_ids** | `list[int]` | List of Target IDs (e.g., `[1, 2]`). Use `[0]` for **Broadcast All**. |
| **data** | `list[int]` | list of 3 integers `[d0, d1, d2]` for extra parameters. |

#### Command Mapping Table

The following commands are supported by the firmware:

| Command | Hex Code | Description | Data Parameter Usage |
| --- | --- | --- | --- |
| **PLAY** | `0x01` | Start timeline/playback. | `[0, 0, 0]` |
| **PAUSE** | `0x02` | Pause playback. | `[0, 0, 0]` |
| **STOP** | `0x03` | Stop and reset position. | `[0, 0, 0]` |
| **RELEASE** | `0x04` | Release memory/Unload. | `[0, 0, 0]` |
| **TEST** | `0x05` | Test Mode / LED Color. | `[R, G, B]` (0-255) or `[0,0,0]` for default pattern. |
| **CANCEL** | `0x06` | Cancel a pending command. | `[cmd_id, 0, 0]` (Use the ID returned by send_burst) |

#### Return Value

Returns a dictionary containing the `command_id` assigned by the ESP32:

```json
{
    "from": "Host_PC",
    "topic": "command",
    "statusCode": 0,
    "payload": {
        "target_id": "[]",
        "command": "PLAY",
        "command_id": "1",
        "message": "Success"
    }
}
```

### Method: `trigger_check`

Sends a `CHECK` command. The ESP32 will broadcast a ping to all devices and then immediately switch to **Scanning Mode** to listen for responses.

```python
trigger_check(target_ids=[])
```

* **Non-blocking**: This function returns immediately so you can continue your script.
* **Background Process**: The ESP32 will scan for exactly **2 seconds**.

### Method: `get_latest_report`

Retrieves the results of the scan initiated by `trigger_check`.

```python
get_latest_report()
```

#### Return Value (Example)

```json
{
    "from": "Host_PC",
    "topic": "check_report",
    "statusCode": 0,
    "payload": {
        "scan_duration_sec": 2,
        "found_count": 1,
        "found_devices": [
            {
                "target_id": 1,          
                "cmd_id": 1,             
                "cmd_type": 1,           
                "target_delay": 5398880, 
                "state": "UNLOADED"      
            }
        ]
    }
}
```
### Class: `Esp32TcpServer` (TCP File Sender)

```python
__init__(control_paths_list, frame_paths_list, host='0.0.0.0', port=3333)
```

* **control_paths_list** (Required): A list of file paths pointing to control.dat for each player. Index 0 corresponds to Player 1, index 1 to Player 2, etc.
* **frame_paths_list**(Required): A list of file paths pointing to frame.dat for each player. Indexing matches control_paths_list.
* **host**: The IP address to bind the server to. Default is '0.0.0.0' (listens on all available network interfaces).
* **port**: The port number to listen for incoming connections. Default is 3333.

## Constraints & Best Practices

### 1. The "Radio Blind Spot"

The ESP32 has only one radio. It **cannot broadcast and scan simultaneously**.

* When you call `trigger_check()`, the ESP32 enters **Observer Mode** for 2 seconds.
* Any `send_burst` commands sent *during* this 2-second window will be **queued** and broadcast only *after* the scan finishes.

### 2. Queue Limit (Max 16)

The firmware scheduler can hold a maximum of **16 pending commands**.

* **Do not** send >16 commands instantly (e.g., in a tight loop).
* If the queue is full, the ESP32 will return `NAK` and `statusCode` will be `-1`.

## Example Usage

Run the example script to see the flow of scheduling commands and checking status.

```bash
python .\examples\lps_ctrl_ex.py
```
Return json:

```json
{
    "from": "Host_PC",
    "topic": "command",
    "statusCode": 0,
    "payload": {
        "target_id": "[]",
        "command": "PLAY",
        "command_id": "0",
        "message": "Success"
    }
}
{
    "from": "Host_PC",
    "topic": "command",
    "statusCode": 0,
    "payload": {
        "target_id": "[]",
        "command": "PAUSE",
        "command_id": "1",
        "message": "Success"
    }
}
{
    "from": "Host_PC",
    "topic": "command",
    "statusCode": 0,
    "payload": {
        "target_id": "[]",
        "command": "CANCEL",
        "command_id": "2",
        "message": "Success"
    }
}
{
    "from": "Host_PC",
    "topic": "check_trigger",
    "statusCode": 0,
    "payload": {
        "target_id": "[]",
        "command": "CHECK",
        "command_id": "3",
        "message": "Check started (ID: 3)"
    }
}
{
    "from": "Host_PC",
    "topic": "command",
    "statusCode": 0,
    "payload": {
        "target_id": "[]",
        "command": "STOP",
        "command_id": "4",
        "message": "Success"
    }
}
{
    "from": "Host_PC",
    "topic": "command",
    "statusCode": 0,
    "payload": {
        "target_id": "[]",
        "command": "TEST",
        "command_id": "5",
        "message": "Success"
    }
}
{
    "from": "Host_PC",
    "topic": "check_report",
    "statusCode": 0,
    "payload": {
        "scan_duration_sec": 2,
        "found_count": 1,
        "found_devices": [
            {
                "target_id": 1,
                "cmd_id": 4,
                "cmd_type": 3,
                "target_delay": 5405590,
                "state": "READY"
            }
        ]
    }
}
```