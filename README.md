# ESP32 Advertiser

This project includes the ESP32 firmware (`adv_esp`) and a PC-side Python control package (`lps-ctrl`).

The system sends commands from a PC via UART to the ESP32. The ESP32 transmits these commands to the receivers (LPS, Light Playback System) via BLE Advertising, and collects status feedback via BLE Scanning.

## System Architecture

### Communication Flow Diagram

```mermaid
graph TD
    PC["Host PC (Python)"] <-->|"UART (Command / Report)"| ESP32["ESP32 Advertiser"]
    
    %% TCP/WiFi OTA Update Path (修正這裡的箭頭)
    PC -.->|"Wi-Fi / TCP (OTA File Transfer)"| LPS1
    PC -.->|"Wi-Fi / TCP (OTA File Transfer)"| LPSn

    %% BLE Control Path
    ESP32 -->|"BLE Advertising (Burst)"| LPS1["LPS Device 1"]
    ESP32 -->|"BLE Advertising (Burst)"| LPS2["LPS Device 2"]
    ESP32 -->|"BLE Advertising (Burst)"| LPSn["LPS Device n"]
    
    %% Feedback Loop
    LPS1 -.->|"BLE ACK (Status)"| ESP32
    LPS2 -.->|"BLE ACK (Status)"| ESP32
    LPSn -.->|"BLE ACK (Status)"| ESP32
```

1. **PC Control Layer (`lps-ctrl`)**

* Encapsulates commands into a CSV format containing the "Expected Arrival Time (Delay)."
* **Non-blocking Control**: Implements an asynchronous architecture to trigger status checks without blocking the main command stream.
* Parses streaming status reports (`FOUND:...`) returned by the ESP32.

2. **Hardware Bridge Layer (`adv_esp`)**

* Receives PC commands via UART (baud rate: 115200).
* **Hybrid Role**: Acts primarily as a Broadcaster, but switches to **Observer (Scanner) Mode** when a `CHECK` command is issued.
* Utilizes **Raw HCI** to bypass the standard Bluetooth stack for precise timing control.

3. **Wireless Broadcasting Layer (BLE Interface)**

* **Burst Mechanism**: Continuously transmits synchronization packets before the target time arrives.
* **LPS Receiver**: Syncs time upon receiving *any* single packet.
* **Status Feedback**: Receivers respond with an ACK packet containing their current state and remaining delay when queried, allowing the PC to monitor fleet status.

4. **Content Update Layer (TCP/Wi-Fi)**

* **TCP Server**: The PC runs an asynchronous TCP server (`Esp32TcpServer`) to host content files.
* **Mode Switching**: Upon receiving the `UPLOAD` command via BLE, LPS Receivers automatically disable their BLE hardware and enable Wi-Fi to establish a direct connection with the PC.
* **Direct Update**: Receivers download specific Control (`control.dat`) and Frame (`frame.dat`) data files based on their Player ID, save them to the SD card, and automatically reboot to apply the new shows.

## Project Structure

```text
├── adv_esp/                # ESP32 BLE Advertiser
│   ├── CMakeLists.txt      
│   └── main/
│       ├── CMakeLists.txt  
│       ├── main.c          # UART handling, command parsing, task scheduling
│       ├── bt_sender.c     # BLE HCI control (TX & RX), packet assembly
│       └── bt_sender.h     
├── lps-ctrl/               # Python control package
│   ├── examples/           # Usage examples (e.g., lps_ctrl_ex.py, tcp_example.py)
│   ├── src/lps_ctrl/       # Core source code
│   │   ├── lps_ctrl.py     # UART Control Logic (ESP32BTSender)
│   │   └── tcp_sender.py   # TCP File Server Logic (Esp32TcpServer)
│   ├── pyproject.toml      # Configuration file
│   └── README.md           
```