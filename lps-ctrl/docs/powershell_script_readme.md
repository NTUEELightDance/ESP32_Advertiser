# ESP32 BLE LPS Controller CLI

這是一個基於 PowerShell 的命令列工具，用於透過 BLE（藍牙低功耗）向 ESP32 LPS 控制器廣播指令。

本腳本透過呼叫 Windows WinRT API 發送藍牙 Service Data (`0x16`) 廣播封包，並利用 .NET 反射（Reflection）技術直接呼叫 CLR，繞過了 PowerShell 5.1 在處理 WinRT 集合型別時常見的轉型錯誤。

## 系統需求

* **作業系統**: Windows 10 或 Windows 11（需支援 WinRT API）。
* **硬體**: 支援藍牙的網卡或接收器（執行前請確認 Windows 藍牙功能已開啟）。
* **環境**: PowerShell 5.1 或更高版本（建議使用系統管理員身分執行以確保藍牙廣播權限）。

## 參數說明

執行腳本時，可以透過以下參數來客製化廣播指令：

| 參數 | 縮寫 | 型別 | 預設值 | 說明 |
| --- | --- | --- | --- | --- |
| `-CmdType` | `-c` | Int | **(必填)** | 指令類型。有效值：`1`:PLAY, `2`:PAUSE, `3`:STOP, `4`:RELEASE, `5`:TEST, `6`:CANCEL, `7`:CHECK, `8`:UPLOAD, `9`:RESET |
| `-TargetId` | `-t` | Int | `-1` | 目標設備 ID (`0-63`)。若設為 `-1` 則代表全域廣播 (Global Broadcast)。 |
| `-CmdId` |  | Int | `0` | 指令的 ID (`0-15`)。 |
| `-DelayMs` |  | Int | `2000` | 延遲時間，單位為毫秒 (ms)。 |
| `-PrepMs` |  | Int | `1000` | 準備時間，單位為毫秒 (ms)。**僅適用於 `PLAY` (Type 1) 指令。** |
| `-R` |  | Byte | `0` | 紅色色值 (`0-255`)。**僅適用於 `TEST` (Type 5) 指令。** |
| `-G` |  | Byte | `0` | 綠色色值 (`0-255`)。**僅適用於 `TEST` (Type 5) 指令。** |
| `-B` |  | Byte | `0` | 藍色色值 (`0-255`)。**僅適用於 `TEST` (Type 5) 指令。** |
| `-CancelId` |  | Int | `0` | 要取消的 Slot ID。**僅適用於 `CANCEL` (Type 6) 指令。** |

## 使用範例

**1. 全域播放 (PLAY)**
對所有設備發送 PLAY 指令，並使用預設的延遲與準備時間：

```powershell
.\LPS_advertiser.ps1 -CmdType 1 -TargetId -1

```

**2. 指定設備測試 (TEST)**
對 ID 為 2 的設備發送 TEST 指令，並點亮紅色 LED (R:255, G:0, B:0)：

```powershell
.\LPS_advertiser.ps1 -CmdType 5 -TargetId 2 -R 255 -G 0 -B 0

```

**3. 取消特定排程 (CANCEL)**
發送 CANCEL 指令，並指定取消 Slot ID 為 3 的任務：

```powershell
.\LPS_advertiser.ps1 -CmdType 6 -CancelId 3

```

## 封包結構解析 (Payload Structure)

腳本在底層會將參數組裝成 19 Bytes 的 Service Data 封包，結構如下：

* **Byte 0-1**: UUID (`0x01`, `0x02` - 可於腳本內自定義)
* **Byte 2**: 指令資訊 (由 CmdId 的高 4 位元與 CmdType 的低 4 位元組合而成)
* **Byte 3-10**: Target Mask (8 Bytes 遮罩。若為 `-1` 則全為 `0xFF`；若指定目標，則將 Bit 1 左移至對應 ID 位置)
* **Byte 11-14**: Delay 延遲時間 (4 Bytes, Big-Endian)
* **Byte 15-18**: 特定指令負載 (4 Bytes, 依據 `CmdType` 動態決定)：
* `Type 1 (PLAY)`: 傳遞 `PrepMs` (Big-Endian)
* `Type 5 (TEST)`: 傳遞 RGB 數值 (`R`, `G`, `B`)
* `Type 6 (CANCEL)`: 傳遞 `CancelId`



## 故障排除

* **廣播失敗或發生例外錯誤**：請確認 Windows 的藍牙已開啟。如果出現存取被拒的問題，請嘗試以「系統管理員身分」開啟 PowerShell 視窗再執行腳本。
* **ESP32 沒收到訊號**：請檢查 ESP32 端的 UUID 解析是否與腳本開頭宣告的 `$uuid1`  和 `$uuid2` 一致。