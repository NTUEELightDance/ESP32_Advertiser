// bt_sender.c
#include "bt_sender.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bt_hci_common.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#define TX_OFFSET_US 9000 // Estimated time offset for TX in microseconds based on empirical measurements
#define MAX_ACTIVE_TASKS 16

/* --- HCI Command Opcodes & Groups --- */
// Defines raw HCI (Host Controller Interface) opcodes to bypass the standard Bluetooth stack for lower latency.
#ifndef HCI_GRP_HOST_CONT_BASEBAND_CMDS
#define HCI_GRP_HOST_CONT_BASEBAND_CMDS (0x03 << 10)
#endif
#ifndef HCI_GRP_BLE_CMDS
#define HCI_GRP_BLE_CMDS               (0x08 << 10)
#endif
#ifndef HCI_SET_EVT_MASK
#define HCI_SET_EVT_MASK               (0x0001 | HCI_GRP_HOST_CONT_BASEBAND_CMDS)
#endif
#ifndef HCI_BLE_WRITE_SCAN_PARAM
#define HCI_BLE_WRITE_SCAN_PARAM       (0x000B | HCI_GRP_BLE_CMDS)
#endif
#ifndef HCI_BLE_WRITE_SCAN_ENABLE
#define HCI_BLE_WRITE_SCAN_ENABLE      (0x000C | HCI_GRP_BLE_CMDS)
#endif
#ifndef HCIC_PARAM_SIZE_SET_EVENT_MASK
#define HCIC_PARAM_SIZE_SET_EVENT_MASK         (8)
#endif
#define UUID1 -1 // change into real uuid
#define UUID2 -1 // change into real uuid

static const char *TAG = "BT_SENDER";
static volatile int64_t last_measured_latency = 0;
static uint8_t hci_cmd_buf[128];        // Buffer for formatting HCI commands
static bool is_initialized = false;     // Flag to check if BT is initialized
static bool is_checking = false;        // Flag indicating if currently in CHECK (Scanning) mode

// Structure to store an active broadcast task
typedef struct {
    bool active;                // Is this slot currently in use?
    int64_t end_time_us;        // Absolute hardware timestamp when broadcasting should stop
    int64_t prep_led_end_time_us; // Absolute timestamp when preparation LED should turn off (for PLAY commands)
    bt_sender_config_t config;  // The command configuration (cmd, delay, target, etc.)
} active_task_t;

static active_task_t s_tasks[MAX_ACTIVE_TASKS]; // Array of active broadcast tasks (slots)
static SemaphoreHandle_t s_task_mutex = NULL;   // Mutex to protect s_tasks array in RTOS multi-thread environment
static int s_rr_index = 0;                      // Round-Robin Index to ensure fair broadcasting across multiple active commands

/* ========================================================
 * Helper functions to format and send low-level HCI commands
 * ======================================================== */

// Pack and send BLE advertising data
static void hci_cmd_send_ble_set_adv_data(uint8_t cmd_type, uint32_t delay_ms, uint32_t prep_led_ms, uint64_t target_mask, const uint8_t *data) {
    uint8_t raw_adv_data[31];
    uint8_t idx = 0;
    
    // --- Payload Structure: Standard BLE Flags ---
    raw_adv_data[idx++] = 2; // Length: 2 bytes
    raw_adv_data[idx++] = 0x01; // AD Type: Flags
    raw_adv_data[idx++] = 0x06; // LE General Discoverable Mode | BR/EDR Not Supported
    
    // --- Payload Structure: Service Data (0x16) ---
    raw_adv_data[idx++] = 20;   // Length: 20 bytes for the following payload
    raw_adv_data[idx++] = 0x16; // AD Type: Service Data - 16-bit UUID
    
    // 2-byte UUID (Little-Endian)
    raw_adv_data[idx++] = UUID1; 
    raw_adv_data[idx++] = UUID2; 
    
    // 1-byte Command Type (e.g., 0x01 for PLAY)
    raw_adv_data[idx++] = cmd_type;

    // 8-byte Target Mask (Specifies which receivers should execute the command)
    for(int i = 0; i < 8; i++) {
        raw_adv_data[idx++] = (uint8_t)((target_mask >> (i * 8)) & 0xFF);
    }

    // 4-byte Remaining Delay in MS (Big-Endian format)
    raw_adv_data[idx++] = (delay_ms >> 24) & 0xFF;
    raw_adv_data[idx++] = (delay_ms >> 16) & 0xFF;
    raw_adv_data[idx++] = (delay_ms >> 8)  & 0xFF;
    raw_adv_data[idx++] = (delay_ms)       & 0xFF;

    // Command-Specific Variable Payload (up to 4 bytes padding)
    uint8_t base_cmd = cmd_type & 0x0F;
    if (base_cmd == 0x01) { // PLAY: 4 bytes indicating LED preparation time
        raw_adv_data[idx++] = (prep_led_ms >> 24) & 0xFF;
        raw_adv_data[idx++] = (prep_led_ms >> 16) & 0xFF;
        raw_adv_data[idx++] = (prep_led_ms >> 8)  & 0xFF;
        raw_adv_data[idx++] = (prep_led_ms)       & 0xFF;
    } else if (base_cmd == 0x05) { // TEST: 3 bytes for RGB values + 1 byte pad
        raw_adv_data[idx++] = data[0]; // R
        raw_adv_data[idx++] = data[1]; // G
        raw_adv_data[idx++] = data[2]; // B
        raw_adv_data[idx++] = 0x00; 
    } else if (base_cmd == 0x06) { // CANCEL: 1 byte for target slot + 3 bytes pad
        raw_adv_data[idx++] = data[0]; 
        raw_adv_data[idx++] = 0x00;
        raw_adv_data[idx++] = 0x00;
        raw_adv_data[idx++] = 0x00;
    } else { // Generic 4-byte pad for other commands
        raw_adv_data[idx++] = 0x00;
        raw_adv_data[idx++] = 0x00;
        raw_adv_data[idx++] = 0x00;
        raw_adv_data[idx++] = 0x00;
    }

    uint16_t sz = make_cmd_ble_set_adv_data(hci_cmd_buf, idx, raw_adv_data);
    if (esp_vhci_host_check_send_available()) esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

// Set basic advertising parameters (Intervals, Channel Map)
static void hci_cmd_send_ble_set_adv_param(void) {
    uint8_t peer_addr[6] = {0};
    uint16_t sz = make_cmd_ble_set_adv_param(hci_cmd_buf, 0x20, 0x20, 0x03, 0, 0, peer_addr, 0x07, 0);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

// Start BLE Advertising
static void hci_cmd_send_ble_adv_start(void) {
    uint16_t sz = make_cmd_ble_set_adv_enable(hci_cmd_buf, 1);
    if (esp_vhci_host_check_send_available()) esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

// Stop BLE Advertising
static void hci_cmd_send_ble_adv_stop(void) {
    uint16_t sz = make_cmd_ble_set_adv_enable(hci_cmd_buf, 0);
    if (esp_vhci_host_check_send_available()) esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

// Reset HCI Controller
static void hci_cmd_send_reset(void) {
    uint16_t sz = make_cmd_reset(hci_cmd_buf);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void controller_rcv_pkt_ready(void) {}

// Parse incoming packets (Used only during 'CHECK' scan mode to receive status ACKs from receivers)
static int host_rcv_pkt(uint8_t *data, uint16_t len) {
    if(!is_checking) return ESP_OK; // Ignore incoming packets if the sender is not actively polling
    
    // Validate HCI LE Meta Event header (0x04 = Event, 0x3E = LE Meta Event, 0x02 = LE Advertising Report)
    if(data[0] != 0x04 || data[1] != 0x3E || data[3] != 0x02) return ESP_OK;

    uint8_t num_reports = data[4];
    uint8_t* payload = &data[5];
    for(int i = 0; i < num_reports; i++) {
        uint8_t data_len = payload[8];
        uint8_t* adv_data = &payload[9];
        uint8_t offset = 0;
        
        while(offset < data_len) {
            uint8_t ad_len = adv_data[offset++];
            if(ad_len == 0) break;
            uint8_t ad_type = adv_data[offset++];
            
            // Expected ACK Payload Structure:
            // Type 0xFF (Manufacturer Specific), followed by 0xFFFF company ID, then our custom UUID
            if(ad_type == 0xFF && (adv_data[offset] == 0xFF && adv_data[offset + 1] == 0xFF) && (adv_data[offset+2] == UUID1 && adv_data[offset + 3] == UUID2)) { 
                
                // Ensure it is an ACK command (0x07) and length matches expected ACK payload (14 bytes)
                if (adv_data[offset+4] == 0x07 && ad_len == 14) {
                    uint8_t target_id = adv_data[offset+5];
                    uint8_t cmd_id    = adv_data[offset+6];
                    uint8_t cmd_type  = adv_data[offset+7];
                    
                    // Reconstruct 4-byte delay (Big-Endian)
                    uint32_t delay_ms = (adv_data[offset+8] << 24) | (adv_data[offset+9] << 16) | (adv_data[offset+10] << 8) | adv_data[offset+11];
                    uint8_t state = adv_data[offset+12];
                    
                    // Output format expected by the Host PC Python script
                    printf("FOUND:%d,%d,%d,%lu,%d\n", target_id, cmd_id, cmd_type, delay_ms, state);
                }
            }
            
            offset += (ad_len - 1); // Jump to next AD Structure block
        }
        payload += (12 + data_len + 1); // Jump to next report in this LE Meta Event
    }
    return ESP_OK;
}

static esp_vhci_host_callback_t vhci_host_cb = { controller_rcv_pkt_ready, host_rcv_pkt };

// Enable LE Meta Event to receive scan reports
static void hci_cmd_send_set_event_mask(void) {
    uint8_t buf[128];
    uint8_t *p = buf;
    
    // Receive LE Meta Event (Bit 61) -> Mask: 00 00 00 00 00 00 00 20
    uint8_t mask[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20};

    // HCI Command (Opcode: 0x0C01)
    *p++ = 0x01; // HCI_COMMAND_PKT
    *p++ = 0x01; // Opcode LSB (0x01)
    *p++ = 0x0C; // Opcode MSB (0x0C) -> 0x0C01 (Set Event Mask)
    *p++ = 0x08; // Param Len
    memcpy(p, mask, 8);
    
    esp_vhci_host_send_packet(buf, 4 + 8);
}

/* ========================================================
 * Main Scheduler RTOS Task
 * Continuously cycles through active tasks and broadcasts them
 * ======================================================== */
static void broadcast_scheduler_task(void *arg) {
    ESP_LOGD(TAG, "Broadcast Scheduler Started (20ms cycle)");
    
    while (1) {
        // Pause broadcasting if system is currently scanning for ACKs to avoid RF collision
        if (is_checking) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        int active_count = 0;
        int task_index_to_run = -1;

        xSemaphoreTake(s_task_mutex, portMAX_DELAY);
        
        // Phase 1: Clean up expired tasks
        for (int i = 0; i < MAX_ACTIVE_TASKS; i++) {
            if (s_tasks[i].active) {
                if (now_us >= s_tasks[i].end_time_us) {
                    s_tasks[i].active = false; // Expire task once its target time is reached
                } else {
                    active_count++;
                }
            }
        }
        
        // Phase 2: Find the next task to broadcast using Round-Robin (to ensure fairness among multiple commands)
        if (active_count > 0) {
            for (int k = 0; k < MAX_ACTIVE_TASKS; k++) {
                int idx = (s_rr_index + k) % MAX_ACTIVE_TASKS;
                if (s_tasks[idx].active) {
                    task_index_to_run = idx;
                    s_rr_index = (idx + 1) % MAX_ACTIVE_TASKS;
                    break;
                }
            }
        }
        xSemaphoreGive(s_task_mutex);
        
        // Phase 3: Execute the chosen broadcast task
        if (task_index_to_run != -1) {
            active_task_t *t = &s_tasks[task_index_to_run];
            
            // Recalculate remaining delay dynamically right before sending
            int32_t remain_ms = (int32_t)((t->end_time_us - now_us) / 1000);
            if (remain_ms < 0) remain_ms = 0;

            int32_t remain_prep_led_ms = (int32_t)((t->prep_led_end_time_us - now_us) / 1000);
            if (remain_prep_led_ms < 0) remain_prep_led_ms = 0;

            hci_cmd_send_ble_set_adv_data(t->config.cmd_type, remain_ms, remain_prep_led_ms, t->config.target_mask, t->config.data);

            // Non-RTOS delay (Busy-wait): Needed to let BT hardware digest the new ADV payload.
            // We MUST use esp_rom_delay_us() here because 500us is smaller than the FreeRTOS minimum tick resolution (typically 1ms).
            // Using vTaskDelay() would force a minimum 1ms yield, introducing jitter and slowing down the strict broadcast rhythm.
            esp_rom_delay_us(500); 
            
            hci_cmd_send_ble_adv_start();
            vTaskDelay(pdMS_TO_TICKS(10)); // Advertise the command for exactly 10ms
            hci_cmd_send_ble_adv_stop();
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Base loop cycle delay to yield CPU
    }
}

// Initialize the Bluetooth Sender component
esp_err_t bt_sender_init(void) {
    if (is_initialized) return ESP_OK;

    // NVS Initialization (Required for BT controller to store calibration data)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Controller Initialization directly at the HCI level (bypassing host stack)
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_vhci_host_register_callback(&vhci_host_cb);

    vTaskDelay(1000 / portTICK_PERIOD_MS);

    // Initial HCI Commands Setup
    hci_cmd_send_reset();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    hci_cmd_send_set_event_mask();
    hci_cmd_send_ble_set_adv_param();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    // Init Mutex and launch Scheduler Task
    s_task_mutex = xSemaphoreCreateMutex();
    for(int i=0; i<MAX_ACTIVE_TASKS; i++) s_tasks[i].active = false;
    xTaskCreate(broadcast_scheduler_task, "bt_scheduler", 4096, NULL, 10, NULL);

    is_initialized = true;
    ESP_LOGI(TAG, "BT Sender API Initialized");
    return ESP_OK;
}

// Add a new command to the broadcast scheduler
int bt_sender_add_task(const bt_sender_config_t *config) {
    if (!is_initialized) return 0;
    int slot = -1;
    
    xSemaphoreTake(s_task_mutex, portMAX_DELAY);
    // Find an empty slot in the task array
    for (int i = 0; i < MAX_ACTIVE_TASKS; i++) {
        if (!s_tasks[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot != -1) {
        s_tasks[slot].config = *config;
        s_tasks[slot].prep_led_end_time_us = esp_timer_get_time() + ((uint64_t)config->prep_led_ms * 1000ULL);
        // Lock in the absolute execution time
        s_tasks[slot].end_time_us = esp_timer_get_time() + ((uint64_t)config->delay_ms * 1000ULL); 
        s_tasks[slot].active = true;
        ESP_LOGD(TAG, "Task added to slot %d (Type 0x%02X)", slot, config->cmd_type);
    } else {
        ESP_LOGW(TAG, "Task List Full! Dropping CMD 0x%02X", config->cmd_type);
    }
    xSemaphoreGive(s_task_mutex);
    
    return (slot != -1) ? 1 : 0; // Return 1 on success, 0 on queue full
}

// Suspend broadcasting and enter scan mode to receive status ACKs
void bt_sender_start_check(uint32_t duration_ms) {
    if (!is_initialized) return;

    is_checking = true;

    // Halt outgoing commands
    hci_cmd_send_ble_adv_stop();
    vTaskDelay(pdMS_TO_TICKS(20));

    // Configure Scan Parameters (Interval 100ms, Window 100ms for continuous capture)
    uint8_t buf[128];
    make_cmd_ble_set_scan_params(buf, 0, 0x00A0, 0x00A0, 0, 0); 
    esp_vhci_host_send_packet(buf, 7 + 4); 
    vTaskDelay(pdMS_TO_TICKS(20));

    // Enable Scanning
    make_cmd_ble_set_scan_enable(buf, 1, 0);
    esp_vhci_host_send_packet(buf, 2 + 4);

    // Wait for the duration (collecting FOUND packets in ISR via host_rcv_pkt)
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    // Disable Scanning
    make_cmd_ble_set_scan_enable(buf, 0, 0);
    esp_vhci_host_send_packet(buf, 2 + 4);

    is_checking = false;
    
    // Signal PC Python script that scan is finished via stdout
    printf("CHECK_DONE\n"); 
}

// Remove/Cancel a specific command slot (used by CANCEL command)
void bt_sender_remove_task(int slot_idx) {
    if (slot_idx >= 0 && slot_idx < MAX_ACTIVE_TASKS) {
        xSemaphoreTake(s_task_mutex, portMAX_DELAY);
        if (s_tasks[slot_idx].active) {
            s_tasks[slot_idx].active = false;
            ESP_LOGD(TAG, "Sender: Stopped broadcasting Task Slot %d", slot_idx);
        }
        xSemaphoreGive(s_task_mutex);
    }
}