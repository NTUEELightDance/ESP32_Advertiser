import asyncio
import os

from lps_ctrl import Esp32TcpServer

async def main():
    # 1. Define total number of players
    NUM_PLAYERS = 32
    
    # 2. Define base directory for player data
    # (Update this path to your actual data location)
    BASE_DIR = r"C:\Users\yingr\Lightdance2026\ESP32_Advertiser\lps-ctrl\src\lps_ctrl\test_data"
    
    all_control_paths = []
    all_frame_paths = []

    print("Generating file paths for all players...")
    
    # 3. Generate file paths for Player_1 to Player_32
    for i in range(1, NUM_PLAYERS + 1):
        # Assuming folder naming convention: Player_1, Player_2 ... Player_32
        player_dir = os.path.join(BASE_DIR, f"Player_{i}")
        
        control_path = os.path.join(player_dir, "control.dat")
        frame_path = os.path.join(player_dir, "frame.dat")
        
        all_control_paths.append(control_path)
        all_frame_paths.append(frame_path)
        
        # Uncomment the line below to debug generated paths:
        # print(f"Player {i} -> {control_path}")

    # 4. Initialize server with the generated path lists
    server = Esp32TcpServer(
        control_paths_list=all_control_paths,
        frame_paths_list=all_frame_paths,
        port=3333
    )

    print("\nStarting project server...")
    # 5. Start serving requests
    await server.start()

if __name__ == '__main__':
    try:
        # Run the async event loop
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServer manually stopped.")