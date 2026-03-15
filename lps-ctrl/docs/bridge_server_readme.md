# ESP32 LPS Gateway: BLE GATT Server to Serial Bridge

This repository contains a cross-platform Python script (`bridge_server.py`) that acts as a communication bridge between a mobile device (BLE GATT Client) and an ESP32 hardware sender connected via USB Serial.

It establishes a local BLE GATT Server on your PC, receives highly compressed custom byte payloads from a mobile app, translates them into the required format, and forwards them to the ESP32 Sender via UART. The ESP32 Sender then broadcasts low-latency hardware-level BLE commands to multiple remote ESP32 Receiver nodes.

---

## 1. System Architecture

The following diagram illustrates the data flow and the role of this Python bridge in the overall system:

```text
[Mobile Phone / Tablet]  (GATT Client)
         |
         |  1. Sends custom HEX payloads (e.g., 0x01...) via BLE GATT
         |  2. Reads/Receives JSON status reports via BLE Notify/Read
         v
[PC / Laptop]            (GATT Server - bridge_server.py)
         |
         |  3. Parses the HEX payload, unpacks Target Masks, and formats the data
         |  4. Forwards the expanded command via USB Serial (UART)
         v
[ESP32 Sender Node]      (Hardware Gateway)
         |
         |  5. Packages data into HCI Connectionless Broadcast (Manufacturer/Service Data)
         |  6. Transmits via BLE Advertising
         v
[ESP32 Receiver Nodes]   (Stage Lights / End Devices)

```

---

## 2. Environment Setup & Installation

### Prerequisites

* **Python:** Python 3.10 is strictly recommended for Windows users to avoid C++ build errors related to the underlying Windows BLE libraries (`bleak-winrt`).
* **Hardware:** An ESP32 flashed with the sender firmware, connected to the host PC via USB.

### Installation Steps

1. Open your terminal or command prompt.
2. Install the required Python packages using `pip`:
```bash
pip install bless pyserial
```


* Note for Windows users: If you encounter an error regarding `pysetupdi` missing, install it directly from the source: `pip install https://github.com/gwangyi/pysetupdi/archive/refs/heads/master.zip`

---

## 3. Usage Guide

### Starting the Server

Run the script from the command line, specifying the serial port your ESP32 is connected to.

**Windows Example:**

```bash
python bridge_server.py -p COM3
```

**macOS / Linux Example:**

```bash
python bridge_server.py -p /dev/ttyUSB0
```

### Command Line Arguments

You can customize the server configuration using the following arguments:

| Argument | Short | Default | Description |
| --- | --- | --- | --- |
| `--port` | `-p` | **(Required)** | Serial port connected to the ESP32 Sender. |
| `--baud` | `-b` | `115200` | Serial baud rate. |
| `--name` | `-n` | `LPS_Gateway` | BLE device name broadcasted over the air. |
| `--service-uuid` |  | `0000AAAA-...` | Custom Service UUID. |
| `--ctrl-uuid` |  | `0000AA01-...` | Control Characteristic UUID (Write). |
| `--report-uuid` |  | `0000AA02-...` | Report Characteristic UUID (Read/Notify). |
---

## 4. Mobile App Interaction (GATT Protocol)

Once the server is running, use a BLE scanner app (e.g., nRF Connect) to connect to `LPS_Gateway`.

### Characteristics

* **Control (Write) - `0xAA01`:** Write HEX byte arrays here to trigger actions.
* **Report (Read/Notify) - `0xAA02`:** Read or subscribe to this characteristic to receive JSON-formatted status reports triggered by the `CHECK` command. Ensure your MTU is set to at least 512 bytes on the client app to receive full JSON payloads.

Here is the updated **Payload Structures** section with the specific timing constraints added. You can directly replace this section in your README.

### Payload Structures (Control Characteristic)

The server utilizes a variable-length protocol to minimize BLE overhead. All numerical values must be sent in **Big-Endian** format. The `Target Mask` is an 8-byte (64-bit) bitmask where each bit represents a target ID. Use `FFFFFFFFFFFFFFFF` for a global broadcast.

**Timing Constraints:**

* **Delay MS:** Must be strictly greater than 1000 ms (1 second).
* **Prep LED MS:** Must be exactly 0 (disabled) or strictly greater than 1000 ms (1 second).

**1. PLAY (17 Bytes)**

* Format: `[CMD: 1 byte] + [Delay MS: 4 bytes] + [Prep LED MS: 4 bytes] + [Target Mask: 8 bytes]`
* Example (Global, 2000ms delay, 1000ms prep): `01 000007D0 000003E8 FFFFFFFFFFFFFFFF`

**2. PAUSE / STOP / RELEASE / UPLOAD / RESET (13 Bytes)**

* Format: `[CMD: 1 byte] + [Delay MS: 4 bytes] + [Target Mask: 8 bytes]`
* Example (PAUSE, Global, 2000ms delay): `02 000007D0 FFFFFFFFFFFFFFFF`

**3. TEST (16 Bytes)**

* Format: `[CMD: 1 byte] + [Delay MS: 4 bytes] + [Target Mask: 8 bytes] + [RGB: 3 bytes]`
* Example (TEST, 2000ms delay, ID 1 and 2, Red): `05 000007D0 0000000000000003 FF0000`

**4. CANCEL (14 Bytes)**

* Format: `[CMD: 1 byte] + [Delay MS: 4 bytes] + [Target Mask: 8 bytes] + [Target id: 1 byte]`
* Example (CANCEL, 2000ms delay, Global, id 1): `06 000007D0 FFFFFFFFFFFFFFFF 01`

**5. CHECK (9 Bytes)**

* Format: `[CMD: 1 byte] + [Target Mask: 8 bytes]`
* Example (CHECK, Global): `07 FFFFFFFFFFFFFFFF`