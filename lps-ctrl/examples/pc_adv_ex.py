import time
from winrt.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementPublisher,
    BluetoothLEAdvertisementWatcher,
    BluetoothLEScanningMode,
    BluetoothLEAdvertisementDataSection
)
from winrt.windows.storage.streams import DataWriter, DataReader

# Define command mapping
COMMANDS = {
    1: "PLAY", 2: "PAUSE", 3: "STOP", 4: "RELEASE",
    5: "TEST", 6: "CANCEL", 7: "CHECK", 8: "UPLOAD", 9: "RESET"
}

# Define state mapping (Reference from lps_ctrl.py)
STATE_MAP = {
    0: "UNLOADED", 1: "READY", 2: "PLAYING", 3: "PAUSE", 4: "TEST"
}

# Replace with your actual UUIDs
UUID1 = 0x4C
UUID2 = 0x44

# Global set to filter duplicate reports within a single scan window
seen_devices = set()

def on_advertisement_received(sender, args):
    """
    Callback function triggered when a BLE advertisement is detected.
    Filters for our custom ACK packet (Manufacturer Data 0xFFFF -> LD 0x07)
    """
    global seen_devices
    adv = args.advertisement
    for man_data in adv.manufacturer_data:
        # Check for our specific Company ID (0xFFFF)
        if man_data.company_id == 0xFFFF:
            reader = DataReader.from_buffer(man_data.data)
            buffer = bytearray(man_data.data.length)
            reader.read_bytes(buffer)
            data_bytes = bytes(buffer)
            
            # Verify Magic Bytes (0x4C, 0x44) and ACK Command Type (0x07)
            if len(data_bytes) >= 11 and data_bytes[0] == 0x4C and data_bytes[1] == 0x44 and data_bytes[2] == 0x07:
                player_id = data_bytes[3]
                
                # Deduplication check
                if player_id in seen_devices:
                    return
                seen_devices.add(player_id)
                
                cmd_id = data_bytes[4]
                cmd_type = data_bytes[5]
                delay_ms = int.from_bytes(data_bytes[6:10], byteorder='big')
                state_raw = data_bytes[10]
                
                cmd_name = COMMANDS.get(cmd_type, "UNKNOWN")
                state_name = STATE_MAP.get(state_raw, f"UNKNOWN({state_raw})")
                rssi = args.raw_signal_strength_in_dbm
                
                print(f"   [REPORT] Player {player_id:02d} | State: {state_name} | Locked CMD: {cmd_name} (ID:{cmd_id}) | Remaining Delay: {delay_ms}ms | RSSI: {rssi}dBm")


def create_payload(cmd_id, cmd_type, target_mask, delay_ms=2000, prep_ms=1000, extra_data=b''):
    # 1. Magic Bytes (2 Bytes): "LD" (0x4C, 0x44 for ESP32)
    magic_bytes = b'\x4C\x44'
    
    # 2. CMD Info (1 Byte): High 4-bit is CMD_ID, Low 4-bit is CMD_TYPE
    cmd_info = ((cmd_id & 0x0F) << 4) | (cmd_type & 0x0F)
    
    # 3. Target Mask (8 Bytes, Little Endian)
    mask_bytes = target_mask.to_bytes(8, byteorder='little')
    
    # 4. Delay Time (4 Bytes, Big Endian) in milliseconds (ms)
    delay_bytes = delay_ms.to_bytes(4, byteorder='big')
    
    # 5. Spec Data (4 Bytes) - Pad to 19 Bytes total
    spec_bytes = bytearray(4)
    if cmd_type == 1: # PLAY (Requires 4 Bytes for prep_ms, Big Endian)
        spec_bytes[:] = prep_ms.to_bytes(4, byteorder='big')
    elif cmd_type == 5: # TEST (Requires 3 Bytes for RGB)
        if len(extra_data) >= 3:
            spec_bytes[0:3] = extra_data[0:3]
    elif cmd_type == 6: # CANCEL (Requires 1 Byte for target CMD_ID to cancel)
        if len(extra_data) >= 1:
            spec_bytes[0] = extra_data[0]
            
    # Combine into a 19-byte payload
    return magic_bytes + bytes([cmd_info]) + mask_bytes + delay_bytes + bytes(spec_bytes)

def main():
    global seen_devices
    # Initialize Publisher
    publisher = BluetoothLEAdvertisementPublisher()
    
    # Initialize Watcher for receiving ACKs
    watcher = BluetoothLEAdvertisementWatcher()
    watcher.scanning_mode = BluetoothLEScanningMode.ACTIVE
    watcher.add_received(on_advertisement_received)
    
    current_cmd_id = 0 
    
    print("=== ESP32 BLE LPS Controller (Interactive Mode) ===")
    
    while True:
        try:
            print("\n" + "="*40)
            print("Available Commands:", ", ".join([f"{k}:{v}" for k, v in COMMANDS.items()]))
            
            # --- 1. Enter Command ---
            cmd_input = input("Enter command code (1-9), or 'q' to quit: ").strip()
            if cmd_input.lower() == 'q':
                break
            cmd_type = int(cmd_input)
            if cmd_type not in COMMANDS:
                print("Error: Invalid command code!")
                continue
                
            # --- 2. Enter Target IDs ---
            target_input = input("Enter Target IDs (e.g., 0,1,2), or 'all' to broadcast to all: ").strip()
            target_mask = 0
            
            if target_input.lower() == 'all':
                target_mask = 0xFFFFFFFFFFFFFFFF
            else:
                target_list = target_input.split(',')
                for tid_str in target_list:
                    if tid_str.strip():
                        tid = int(tid_str.strip())
                        if 0 <= tid <= 63:
                            target_mask |= (1 << tid)
                        else:
                            print(f"Warning: Target ID {tid} is out of range (0-63) and will be ignored.")
            
            if target_mask == 0:
                print("Error: No valid targets specified!")
                continue

            # --- 3. Enter Delay and Prep Time ---
            if cmd_type == 7: # CHECK command requires shorter default delay
                delay_input = input("Enter Delay time in ms [Default: 1500 for CHECK]: ").strip()
                delay_ms = int(delay_input) if delay_input else 1500
            else:
                delay_input = input("Enter Delay time in ms [Default: 2000]: ").strip()
                delay_ms = int(delay_input) if delay_input else 2000

            prep_ms = 1000
            if cmd_type == 1: # PLAY only
                prep_input = input("Enter Prep LED time in ms [Default: 1000]: ").strip()
                prep_ms = int(prep_input) if prep_input else 1000

            # --- 4. Handle Special Commands (TEST / CANCEL) ---
            extra_data = b''
            if cmd_type == 5: # LPS_CMD_TEST
                rgb_input = input("Enter RGB values (Format: R,G,B, e.g., 255,0,0) or press Enter for default breathing: ").strip()
                if rgb_input:
                    r, g, b = map(int, rgb_input.split(','))
                    extra_data = bytes([r & 0xFF, g & 0xFF, b & 0xFF])
            elif cmd_type == 6: # LPS_CMD_CANCEL
                cancel_input = input("Enter the CMD_ID to cancel (0-15): ").strip()
                if cancel_input:
                    cancel_id = int(cancel_input)
                    extra_data = bytes([cancel_id & 0x0F])

            # Generate Payload
            payload = create_payload(current_cmd_id, cmd_type, target_mask, delay_ms, prep_ms, extra_data=extra_data)
            
            # --- 5. Update and Broadcast ---
            if publisher.status == 2:
                publisher.stop()
                time.sleep(0.1)
                
            publisher.advertisement.manufacturer_data.clear()
            adv = publisher.advertisement
            adv.data_sections.clear()

            service_uuid = bytes([UUID1, UUID2])
            writer = DataWriter()
            writer.write_bytes(service_uuid + payload[2:])

            section = BluetoothLEAdvertisementDataSection(
                0x16,  # Service Data
                writer.detach_buffer()
            )
            adv.data_sections.append(section)

            # --- 6. Broadcast and Optional Listening ---
            publisher.start()
            
            if cmd_type == 7:
                # For CHECK, broadcast briefly, then switch to listener mode
                time.sleep(1.0) 
                publisher.stop()
                
                listen_time = (delay_ms / 1000.0) + 1.5
                print(f"\n[INFO] Broadcast sent. Listening for device reports for {listen_time:.1f} seconds...")
                
                # Clear the seen devices set before starting a new scan
                seen_devices.clear()
                
                watcher.start()
                time.sleep(listen_time)
                watcher.stop()
                print("[INFO] Listening finished.")
            else:
                # For standard commands, broadcast for 1 full second to ensure detection
                time.sleep(1) 
                publisher.stop()
                print(f"\n[INFO] Broadcast sent!")
            
            print(f"   Command: {COMMANDS[cmd_type]} (CMD_ID: {current_cmd_id})")
            print(f"   Target Mask: {hex(target_mask)}")
            print(f"   Delay: {delay_ms}ms" + (f", Prep: {prep_ms}ms" if cmd_type == 1 else ""))
            print(f"   Raw Payload (Hex): {payload.hex().upper()}")
            
            # Increment CMD_ID (0-15 loop)
            current_cmd_id = (current_cmd_id + 1) % 16

        except ValueError:
            print("Error: Please enter valid numbers! (Check your commas and values)")
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"Unknown error occurred: {e}")

    # Ensure services are stopped on exit
    print("\nStopping services...")
    publisher.stop()
    watcher.stop()
    print("Services stopped. Exiting.")

if __name__ == "__main__":
    main()