import asyncio
import json
import argparse
import sys
from bless import (
    BlessServer,
    BlessGATTCharacteristic,
    GATTCharacteristicProperties,
    GATTAttributePermissions
)
from lps_ctrl import ESP32BTSender 

async def run_server(args):
    loop = asyncio.get_running_loop()
    
    # 1. Initialize ESP32 Sender
    print(f"Connecting to ESP32 Sender (Port: {args.port}, Baud: {args.baud})...")
    try:
        sender = ESP32BTSender(port=args.port, baud_rate=args.baud)
        sender.connect()
        print("ESP32 Sender connected via Serial.")
    except Exception as e:
        print(f"Failed to connect to Serial Port {args.port}: {e}")
        sys.exit(1)

    # 2. Initialize GATT Server
    server = BlessServer(name=args.name)
    await server.add_new_service(args.service_uuid)

    # Add Control Characteristic (Write)
    await server.add_new_characteristic(
        args.service_uuid, 
        args.ctrl_uuid,
        GATTCharacteristicProperties.write,
        bytearray(b"\x00"),
        GATTAttributePermissions.writeable
    )

    # Add Report Characteristic (Read/Notify)
    await server.add_new_characteristic(
        args.service_uuid, 
        args.report_uuid,
        GATTCharacteristicProperties.read | GATTCharacteristicProperties.notify,
        bytearray(b"{}"), 
        GATTAttributePermissions.readable
    )

    # 3. Command Processing Logic
    async def process_command(value: bytearray):
        if len(value) < 1:
            return
        
        cmd_int = value[0]
        cmd_map_inv = {v: k for k, v in sender.CMD_MAP.items()}
        cmd_name = cmd_map_inv.get(cmd_int, None)
        
        if not cmd_name:
            print(f"Unknown command code received: 0x{cmd_int:02X}")
            return

        # Default values
        delay_ms = 0
        prep_ms = 0
        data = [0, 0, 0]
        target_mask = 0xFFFFFFFFFFFFFFFF  # Default to global broadcast

        try:
            # Parse dynamic length payloads based on command type
            if cmd_int == 0x01: # PLAY (17 Bytes)
                # Format: [CMD:1] [Delay:4] [Prep:4] [Mask:8]
                delay_ms = int.from_bytes(value[1:5], byteorder='big')
                prep_ms = int.from_bytes(value[5:9], byteorder='big')
                target_mask = int.from_bytes(value[9:17], byteorder='big')

            elif cmd_int in [0x02, 0x03, 0x04, 0x08, 0x09]: # PAUSE, STOP, RELEASE, UPLOAD, RESET (13 Bytes)
                # Format: [CMD:1] [Delay:4] [Mask:8]
                delay_ms = int.from_bytes(value[1:5], byteorder='big')
                target_mask = int.from_bytes(value[5:13], byteorder='big')

            elif cmd_int == 0x05: # TEST (16 Bytes)
                # Format: [CMD:1] [Delay:4] [Mask:8] [RGB:3]
                delay_ms = int.from_bytes(value[1:5], byteorder='big')
                target_mask = int.from_bytes(value[5:13], byteorder='big')
                data = list(value[13:16]) # Extract RGB

            elif cmd_int == 0x06: # CANCEL (14 Bytes)
                # Format: [CMD:1] [Delay:4] [Mask:8] [Slot:1]
                delay_ms = int.from_bytes(value[1:5], byteorder='big')
                target_mask = int.from_bytes(value[5:13], byteorder='big')
                data[0] = value[13] # Store slot to cancel in data[0]

            elif cmd_int == 0x07: # CHECK (9 Bytes)
                # Format: [CMD:1] [Mask:8]
                target_mask = int.from_bytes(value[1:9], byteorder='big')

        except IndexError:
            print(f"Insufficient payload length! Cannot parse {len(value)} bytes for [{cmd_name}]: {value.hex()}")
            return

        # Convert bitmask back to target ID list for lps_ctrl.py
        if target_mask == 0xFFFFFFFFFFFFFFFF:
            target_ids = None  # Global broadcast
        elif target_mask == 0:
            target_ids = [0]
        else:
            target_ids = []
            for i in range(64):
                if target_mask & (1 << i):
                    target_ids.append(i)

        delay_sec = delay_ms / 1000.0
        prep_led_sec = prep_ms / 1000.0

        print(f"Executing: [{cmd_name}], Delay={delay_sec}s, PrepLED={prep_led_sec}s, Data={data}, Targets={target_ids}")

        # Execute corresponding sender function
        if cmd_name == "CHECK":
            print("Starting CHECK status scan...")
            await asyncio.to_thread(sender.trigger_check, target_ids if target_ids else [])
            report = await asyncio.to_thread(sender.get_latest_report)
            report_str = json.dumps(report)
            print(f"Scan result: {report_str}")
            
            # Update characteristic value for client to read/notify
            report_bytes = bytearray(report_str.encode('utf-8')[:512])
            server.get_characteristic(args.report_uuid).value = report_bytes
            server.update_value(args.service_uuid, args.report_uuid)
        else:
            await asyncio.to_thread(
                sender.send_burst, 
                cmd_input=cmd_name, 
                delay_sec=delay_sec, 
                prep_led_sec=prep_led_sec, 
                target_ids=target_ids, 
                data=data
            )
    
    # 4. Bind BLE event callbacks
    def write_request(char: BlessGATTCharacteristic, value: bytearray, **kwargs):
        if char.uuid.lower() == args.ctrl_uuid.lower():
            # Dispatch command processing safely to the main event loop
            asyncio.run_coroutine_threadsafe(process_command(value), loop)
        char.value = value

    def read_request(char: BlessGATTCharacteristic, **kwargs) -> bytearray:
        if char.uuid.lower() == args.report_uuid.lower():
             report = sender.get_latest_report()
             return bytearray(json.dumps(report).encode('utf-8')[:512])
        return char.value

    server.read_request_func = read_request
    server.write_request_func = write_request

    # 5. Start Server
    await server.start()
    print("==================================================")
    print("GATT Server started!")
    print(f"   Device Name:  {args.name}")
    print(f"   Service UUID: {args.service_uuid}")
    print(f"   Control UUID: {args.ctrl_uuid} (Write)")
    print(f"   Report UUID:  {args.report_uuid} (Read/Notify)")
    print("==================================================")
    print("Press Ctrl+C to stop the server...")

    try:
        while True:
            await asyncio.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        print("\nShutting down system...")
        await server.stop()
        sender.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="ESP32 LPS Gateway - BLE GATT Server to Serial Bridge",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    
    parser.add_argument("-p", "--port", type=str, required=True, 
                        help="Serial port for ESP32 connection (e.g., COM3, /dev/ttyUSB0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, 
                        help="Serial baud rate")
    parser.add_argument("-n", "--name", type=str, default="LPS_Gateway", 
                        help="BLE device name to broadcast")
    parser.add_argument("--service-uuid", type=str, default="0000AAAA-0000-1000-8000-00805F9B34FB", 
                        help="Custom Service UUID")
    parser.add_argument("--ctrl-uuid", type=str, default="0000AA01-0000-1000-8000-00805F9B34FB", 
                        help="Custom Control Characteristic UUID")
    parser.add_argument("--report-uuid", type=str, default="0000AA02-0000-1000-8000-00805F9B34FB", 
                        help="Custom Report Characteristic UUID")

    args = parser.parse_args()
    asyncio.run(run_server(args))