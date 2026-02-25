#include "kf1950.h"

#include "global_state.h"
#include "serial.h"
#include "utils.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "kf1950";

static task_result result;
static uint8_t job_id_counter = 0;
static bool first_job_sent = false; // For WM test job diagnostic

// ---------------------------------------------------------------------------
// CRC-8 (poly 0x31, MSB-first, no reflection, xorOut=0)
// ---------------------------------------------------------------------------
static uint8_t kf1950_crc8(const uint8_t *data, size_t len, uint8_t init)
{
    uint8_t crc = init;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Low-level packet helpers
// ---------------------------------------------------------------------------

// Build and send a KF1950 command packet: FF FF [payload] [CRC8]
static void _send_kf1950(const uint8_t *payload, size_t payload_len, uint8_t crc_init, bool debug)
{
    size_t total_len = 2 + payload_len + 1; // preamble + payload + CRC
    uint8_t buf[total_len];

    buf[0] = 0xFF;
    buf[1] = 0xFF;
    memcpy(buf + 2, payload, payload_len);
    buf[total_len - 1] = kf1950_crc8(buf, total_len - 1, crc_init);

    SERIAL_send(buf, total_len, debug);
}

// Send a command with default CRC init (0xFF)
static void _send_cmd(const uint8_t *payload, size_t len)
{
    _send_kf1950(payload, len, 0xFF, KF1950_SERIALTX_DEBUG);
}

// Send and wait for UART to complete
static void _send_cmd_wait(const uint8_t *payload, size_t len, int delay_ms)
{
    _send_cmd(payload, len);
    vTaskDelay(delay_ms / portTICK_PERIOD_MS);
}

// Count KF1950 chips on the chain
static int _count_chips(uint16_t expected_count)
{
    // Send chip ID read: FF FF 10 00 06 CRC
    uint8_t read_cmd[] = {0x10, 0x00, 0x06};
    _send_cmd(read_cmd, sizeof(read_cmd));

    // Read responses — try reading up to 64 bytes to see what comes back
    uint8_t raw_buf[64];
    int total_received = SERIAL_rx(raw_buf, sizeof(raw_buf), 2000);

    if (total_received <= 0) {
        ESP_LOGW(TAG, "No chip ID response (0 bytes)");
        return 0;
    }

    ESP_LOGI(TAG, "Chip ID raw response (%d bytes):", total_received);
    for (int i = 0; i < total_received; i++) {
        printf("%02X ", raw_buf[i]);
    }
    printf("\n");

    // Parse 11-byte chip responses from the buffer
    int chip_count = 0;
    int offset = 0;

    while (offset + KF1950_CHIP_ID_RESPONSE_LENGTH <= total_received) {
        uint8_t *buf = raw_buf + offset;

        // Verify CRC on response
        uint8_t crc = kf1950_crc8(buf, 10, 0xFF);
        if (crc != buf[10]) {
            ESP_LOGW(TAG, "CRC failed at offset %d: calc=%02X got=%02X", offset, crc, buf[10]);
            offset++;  // Try next byte alignment
            continue;
        }

        ESP_LOGI(TAG, "Chip %d detected: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 chip_count, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
                 buf[6], buf[7], buf[8], buf[9], buf[10]);
        chip_count++;
        offset += KF1950_CHIP_ID_RESPONSE_LENGTH;
    }

    if (chip_count != expected_count) {
        ESP_LOGW(TAG, "%d chip(s) detected, expected %d", chip_count, expected_count);
    }

    return chip_count;
}

// ---------------------------------------------------------------------------
// Diagnostic status read — logs byte[3] at each init checkpoint
// ---------------------------------------------------------------------------
static uint8_t _read_status_diag(const char *label)
{
    SERIAL_clear_buffer();
    _send_cmd((uint8_t[]){0x11, 0x00, 0x01}, 3);

    uint8_t buf[16];
    int n = 0;
    uint32_t t_end = xTaskGetTickCount() + pdMS_TO_TICKS(400);
    while (xTaskGetTickCount() < t_end) {
        int r = SERIAL_rx(buf + n, 1, 50);
        if (r > 0) n += r;
        if (n >= 6) break;
    }

    if (n == 0) {
        ESP_LOGW(TAG, "DIAG [%s]: no response", label);
        return 0xFF;
    }

    char hex[32] = {0};
    int pos = 0;
    for (int i = 0; i < n; i++)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
    uint8_t b3 = (n >= 4) ? buf[3] : 0xFF;
    ESP_LOGI(TAG, "DIAG [%s]: %s  b3=0x%02X  %s",
             label, hex, b3, (b3 == 0x80) ? "<-- PLL LOCKED" : "(not locked)");
    return b3;
}

// ---------------------------------------------------------------------------
// Init sequence
// ---------------------------------------------------------------------------
uint8_t KF1950_init(float frequency, uint16_t asic_count, uint16_t difficulty)
{
    ESP_LOGI(TAG, "Initializing KF1950, target freq: %.0f MHz, chips: %d", frequency, asic_count);

    // PLL N byte: WM CSV uses N=0x80 for ALL PLL writes regardless of frequency target.
    // The frequency parameter is ignored here — 0x80 is the constant WM uses.
    // Dynamic frequency control can be added once basic hashing is verified.
    const uint8_t pll_n = 0x80;
    ESP_LOGI(TAG, "PLL N=0x%02X (WM constant, target freq: %.0f MHz)", pll_n, frequency);

    // Set baud to 363636 for KF1950 communication
    SERIAL_set_baud(KF1950_INIT_BAUD);
    vTaskDelay(500 / portTICK_PERIOD_MS);  // KF1950 needs longer post-reset startup
    SERIAL_clear_buffer();

    // Phase 1: Chip ID read (retry up to 3 times — KF1950 may need extra time after reset)
    int chip_count = 0;
    for (int attempt = 0; attempt < 3 && chip_count == 0; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Chip detect retry %d/3...", attempt + 1);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            SERIAL_clear_buffer();
        }
        chip_count = _count_chips(asic_count);
    }
    if (chip_count == 0) {
        ESP_LOGE(TAG, "No KF1950 chips detected!");
        return 0;
    }

    // Phase 2: Initial config
    _send_cmd_wait((uint8_t[]){0x12, 0x34, 0x02, 0x13, 0xF4}, 5, 10);
    _send_cmd_wait((uint8_t[]){0x07, 0x04, 0x02, 0x15, 0x15}, 5, 10);
    _send_cmd_wait((uint8_t[]){0x00, 0x04, 0x01, 0xAA}, 4, 10);

    // Phase 3: Address assignment (single chip, address 0x01)
    // WM format: FF FF 00 14 02 00 [chip] [hash] 00 [chip] 02 04 01 [b12] CRC
    // Use address 0x01 so the chip receives the core enable (which starts at 0x01 in WM)
    // WM CSV chip 0x01: hash=0xE1, b12=0x30 (CRC init 0x3A → CRC 0xA6, verified)
    uint8_t addr_payload[] = {0x00, 0x14, 0x02, 0x00, 0x01, 0xE1, 0x00, 0x01, 0x02, 0x04, 0x01, 0x30};
    _send_kf1950(addr_payload, sizeof(addr_payload), 0x3A, KF1950_SERIALTX_DEBUG);
    vTaskDelay(10 / portTICK_PERIOD_MS);

    // Drain any address assignment responses
    SERIAL_clear_buffer();

    // Phase 4: Baud divider (internal ASIC setting, does not change UART baud)
    _send_cmd_wait((uint8_t[]){0x07, 0x04, 0x02, 0x35, 0x35}, 5, 10);

    // Phase 5: PLL config — 4 domains × 2 iterations each = 40 commands total.
    // Python ground truth (kf1950_wm_init_job.py) uses 2 iterations per domain.
    for (int domain = 0; domain < 4; domain++) {
        for (int iter = 0; iter < 2; iter++) {
            _send_cmd_wait((uint8_t[]){0x03, 0x04, 0x01, 0x05}, 4, 5);
            uint8_t pll_cfg[] = {0x03, 0x14, 0x06, 0x00, pll_n, 0x04, 0x05, (uint8_t)domain, 0xAA};
            _send_cmd_wait(pll_cfg, sizeof(pll_cfg), 5);
            _send_cmd_wait((uint8_t[]){0x03, 0x04, 0x01, 0x04}, 4, 5);
            _send_cmd_wait((uint8_t[]){0x03, 0x04, 0x01, 0x04}, 4, 5);
            _send_cmd_wait(pll_cfg, sizeof(pll_cfg), 5);
        }
    }

    // Diagnostic: check PLL lock state after Phase 5
    _read_status_diag("after_pll_config");

    // Phase 6: Core enables — 80 44 / 8D D4 FIRST, then all 6 masks × 1 rep, ONE reset at end.
    // Ground truth: kf1950_wm_init_job.py (produced 19,612 nonces on aditBoard).
    _send_cmd_wait((uint8_t[]){0x80, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x20, 0x00, 0x40, 0x00, 0x60, 0x00, 0x00, 0x00, 0x30}, 15, 10);
    _send_cmd_wait((uint8_t[]){0x8D, 0xD4, 0x01, 0xAA}, 4, 10);
    {
        static const uint8_t all_masks[] = {0x31, 0x21, 0x11, 0x91, 0xA1, 0xB1};
        for (int i = 0; i < 6; i++) {
            uint8_t core_en[] = {0x04, 0x04, 0x04, 0x01, 0x3F, all_masks[i], 0xAA};
            _send_cmd_wait(core_en, sizeof(core_en), 5);
        }
        // ONE core reset at the end (not between masks)
        _send_cmd_wait((uint8_t[]){0x04, 0x24, 0x01, 0x00}, 4, 5);
        _send_cmd_wait((uint8_t[]){0x04, 0x24, 0x01, 0x01}, 4, 5);
    }

    // Diagnostic: check PLL lock state after core enables
    _read_status_diag("after_core_enables");

    // Phase 7: Post-config registers (NO 22 04, NO 24 94 — not in Python ground truth)
    _send_cmd_wait((uint8_t[]){0x02, 0x14, 0x01, 0x3A}, 4, 5);
    _send_cmd_wait((uint8_t[]){0x23, 0x04, 0x06, 0x26, 0x1F, 0x30, 0x80, 0x00, 0xAA}, 9, 5);
    _send_cmd_wait((uint8_t[]){0x24, 0x04, 0x07, 0x25, 0xFE, 0x50, 0x00, 0x60, 0x00, 0x00}, 10, 5);

    // Status read (matches Python script position)
    _read_status_diag("after_post_config");

    // 05 14 04 WITH CRC (Python uses make_pkt = proper packet with CRC appended)
    _send_cmd_wait((uint8_t[]){0x05, 0x14, 0x04}, 3, 10);

    // Trigger: raw FF FF E3 (3 bytes, no CRC — Python writes raw bytes, not make_pkt)
    {
        uint8_t trigger[] = {0xFF, 0xFF, 0xE3};
        SERIAL_send(trigger, 3, KF1950_SERIALTX_DEBUG);
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    _send_cmd_wait((uint8_t[]){0x05, 0x04, 0x01, 0x83}, 4, 10);
    _send_cmd_wait((uint8_t[]){0x06, 0x04, 0x01, 0x11}, 4, 10);

    // pll_phase2 then B1 second pass (Python: 03 04 01 04 + 03 14 06 00 80 04 02 03 AA, then B1)
    _send_cmd_wait((uint8_t[]){0x03, 0x04, 0x01, 0x04}, 4, 5);
    {
        uint8_t pll_phase2[] = {0x03, 0x14, 0x06, 0x00, pll_n, 0x04, 0x02, 0x03, 0xAA};
        _send_cmd_wait(pll_phase2, sizeof(pll_phase2), 5);
    }
    _send_cmd_wait((uint8_t[]){0x04, 0x04, 0x04, 0x01, 0x3F, 0xB1, 0xAA}, 7, 5);

    // Final PLL — WM ground truth: N=pll_n, field=0x04, domains 0x00 and 0x0F
    _send_cmd_wait((uint8_t[]){0x03, 0x04, 0x01, 0x04}, 4, 5);
    {
        uint8_t final_pll1[] = {0x03, 0x14, 0x06, 0x00, pll_n, 0x04, 0x04, 0x00, 0xAA};
        _send_cmd_wait(final_pll1, sizeof(final_pll1), 5);
    }
    _send_cmd_wait((uint8_t[]){0x20, 0x04, 0x01, 0x04}, 4, 5);
    {
        uint8_t final_pll2[] = {0x20, 0x14, 0x06, 0x00, pll_n, 0x04, 0x04, 0x0F, 0xAA};
        _send_cmd_wait(final_pll2, sizeof(final_pll2), 5);
    }

    // Diagnostic: check PLL lock state after final PLL writes
    _read_status_diag("after_final_pll");

    // CRITICAL: Skip final baud commands — they change ASIC baud to unknown rate
    // data.write(make_pkt(bytes([0x01, 0x44, 0x02, 0x04, 0x00]))); — SKIP
    // data.write(make_pkt(bytes([0x01, 0x04, 0x04, 0x01, 0x00, 0x2D, 0xAA]))); — SKIP

    ESP_LOGI(TAG, "KF1950 init complete, %d chip(s)", chip_count);

    SERIAL_clear_buffer();
    return chip_count;
}

// ---------------------------------------------------------------------------
// Job packet construction (228 bytes)
// ---------------------------------------------------------------------------

// Send WM-captured test job (Job 0) to verify ASIC is hashing after init.
// This is a known-good packet that produced nonces in aditBoard testing.
static void _send_wm_test_job(void)
{
    // WM Job 0: midstate at slot 0, job_id=0x80, counter=0xEE
    static const uint8_t wm_job0_midstate[32] = {
        0x76, 0xB0, 0xFF, 0x42, 0x49, 0x29, 0xDC, 0xD9,
        0x36, 0xA0, 0x41, 0x81, 0xEB, 0xF1, 0xCF, 0x0D,
        0x5E, 0xA6, 0xE7, 0x09, 0xFE, 0x9B, 0x84, 0x00,
        0x7E, 0x04, 0x0E, 0xFF, 0x63, 0xE8, 0x45, 0x8E
    };

    uint8_t pkt[KF1950_JOB_PACKET_SIZE];
    memset(pkt, 0x55, KF1950_JOB_PACKET_SIZE);

    // Header
    pkt[0] = 0xFF; pkt[1] = 0xFF; pkt[2] = 0x80; pkt[3] = 0x04; pkt[4] = 0xDE;
    // Job metadata (exact WM values)
    pkt[5]  = 0x80; // job_id
    pkt[6]  = 0x76; // b6
    pkt[7]  = 0x42; // b7
    pkt[8]  = 0x00; // flag
    pkt[9]  = 0x00;
    pkt[10] = 0x58; // slot 0 marker
    // Derived pattern: b7=0x42, DD=(0x58+0x20)=0x78
    pkt[11] = 0x42; pkt[12] = 0x78;
    pkt[13] = 0x42; pkt[14] = 0x78;
    pkt[15] = 0x42; pkt[16] = 0x78;
    // Midstate in slot 0
    memcpy(pkt + 21, wm_job0_midstate, 32);
    // Tail
    pkt[213] = 0x56;
    pkt[214] = 0xB1; pkt[215] = 0x0E; pkt[216] = 0x17; pkt[217] = 0x59; // ntime
    pkt[218] = 0x15; pkt[219] = 0xD3; pkt[220] = 0x5F; // nbits
    pkt[221] = 0x52; pkt[222] = 0x88; pkt[223] = 0x36; pkt[224] = 0x7F; // nonce_start
    pkt[225] = 0xEE; // counter
    pkt[226] = 0xAA;
    pkt[227] = kf1950_crc8(pkt, 227, 0xFF);

    // Pre-job STATUS_READ: confirm chip is alive right before we send the job
    SERIAL_clear_buffer();
    _send_cmd((uint8_t[]){0x11, 0x00, 0x01}, 3);
    {
        uint8_t pre_buf[16]; int pre_n = 0;
        uint32_t t = xTaskGetTickCount() + pdMS_TO_TICKS(500);
        while (xTaskGetTickCount() < t) {
            int r = SERIAL_rx(pre_buf + pre_n, 1, 50);
            if (r > 0) pre_n += r;
            if (pre_n >= 6) break;
        }
        if (pre_n > 0) {
            char hex[32] = {0}; int pos = 0;
            for (int i = 0; i < pre_n; i++) pos += snprintf(hex+pos, sizeof(hex)-pos, "%02X ", pre_buf[i]);
            ESP_LOGW(TAG, "PRE-JOB alive: %s (chip alive before job)", hex);
        } else {
            ESP_LOGE(TAG, "PRE-JOB SILENT: chip not responding BEFORE job — something ate the bytes?");
        }
    }

    ESP_LOGW(TAG, ">>> SENDING WM TEST JOB (should produce nonces if init is correct) <<<");
    SERIAL_send(pkt, KF1950_JOB_PACKET_SIZE, KF1950_DEBUG_WORK);

    // Step 1: Collect raw bytes for 2 seconds after job send (nonce window)
    {
        uint8_t raw[256];
        int total = 0;
        uint32_t t_end = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (xTaskGetTickCount() < t_end) {
            int n = SERIAL_rx(raw + total, 1, 50);
            if (n > 0) total += n;
            if (total >= (int)sizeof(raw)) break;
        }
        if (total > 0) {
            ESP_LOGW(TAG, "RAW RX after test job (%d bytes):", total);
            for (int i = 0; i < total; i += 16) {
                int end = (i + 16 < total) ? i + 16 : total;
                char hex[64] = {0};
                int pos = 0;
                for (int j = i; j < end; j++)
                    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw[j]);
                ESP_LOGW(TAG, "  [%03d] %s", i, hex);
            }
        } else {
            ESP_LOGE(TAG, "RAW RX: ZERO BYTES after job (2s) — chip not hashing!");
        }
    }

    // Step 2: Send a status read to check if ASIC is still alive and responding
    // If chip is alive: response arrives (may be stolen by asic_result_task, which
    // will log "Invalid nonce header: 00 ..." — watch for that in log).
    // If chip is dead/silent: nothing arrives at all.
    ESP_LOGW(TAG, ">>> Sending status read — watching if chip is still alive <<<");
    _send_cmd((uint8_t[]){0x11, 0x00, 0x01}, 3);

    {
        uint8_t raw2[64];
        int total2 = 0;
        uint32_t t_end2 = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        while (xTaskGetTickCount() < t_end2) {
            int n = SERIAL_rx(raw2 + total2, 1, 50);
            if (n > 0) total2 += n;
            if (total2 >= (int)sizeof(raw2)) break;
        }
        if (total2 > 0) {
            char hex[160] = {0};
            int pos = 0;
            for (int j = 0; j < total2 && pos < (int)sizeof(hex)-4; j++)
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", raw2[j]);
            ESP_LOGW(TAG, "ALIVE: status read got %d bytes: %s", total2, hex);
        } else {
            ESP_LOGE(TAG, "SILENT: chip did NOT respond to status read — hardware/power issue?");
        }
    }
}

void KF1950_send_work(void *pvParameters, bm_job *next_bm_job)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    // On first call, send a known-good WM test job to verify ASIC is hashing
    if (!first_job_sent) {
        first_job_sent = true;
        _send_wm_test_job();
        // Wait a bit for test job nonces before sending real work
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    uint8_t pkt[KF1950_JOB_PACKET_SIZE];
    memset(pkt, 0x55, KF1950_JOB_PACKET_SIZE); // fill with padding

    job_id_counter = (job_id_counter + 1) & 0xFF;

    // Bytes 0-4: Header
    pkt[0] = 0xFF;
    pkt[1] = 0xFF;
    pkt[2] = 0x80; // JOB command
    pkt[3] = 0x04;
    pkt[4] = 0xDE;

    // Byte 5: Job ID — bit 7 MUST be set (WM always uses 0x80+, chip ignores jobs without it)
    pkt[5] = job_id_counter | 0x80;

    // Bytes 6-7: Merkle root tail bytes
    // bm_job->merkle_root is word-reversed, so bytes [0:4] = original tail bytes [28:32]
    // WM data shows bytes 6-7 vary per job — these are likely merkle_root_tail[2:4]
    // (the last 2 bytes of the original merkle root)
    pkt[6] = next_bm_job->merkle_root[2];
    pkt[7] = next_bm_job->merkle_root[3];

    // Byte 8: Flag (0x00 for slot 0)
    pkt[8] = 0x00;

    // Byte 9: 0x00 (standard for early job format)
    pkt[9] = 0x00;

    // Byte 10: Slot marker (slot 0 = 0x58)
    pkt[10] = 0x58;

    // Bytes 11-16: Derived pattern
    // Formula: DD = (B10 + 0x20) & 0xFF when flag=0x00
    uint8_t dd = (pkt[10] + 0x20) & 0xFF;
    pkt[11] = pkt[7]; pkt[12] = dd;
    pkt[13] = pkt[7]; pkt[14] = dd;
    pkt[15] = pkt[7]; pkt[16] = dd;

    // Bytes 17-20: Padding (already 0x55 from memset)

    // Bytes 21-52: Midstate in slot 0
    memcpy(pkt + 21, next_bm_job->midstate, 32);

    // Slots 1-5: Padding (already 0x55 from memset)

    // Byte 213: Tail marker
    pkt[213] = 0x56;

    // Bytes 214-217: ntime (little-endian, matches WM format)
    pkt[214] = (next_bm_job->ntime >>  0) & 0xFF;
    pkt[215] = (next_bm_job->ntime >>  8) & 0xFF;
    pkt[216] = (next_bm_job->ntime >> 16) & 0xFF;
    pkt[217] = (next_bm_job->ntime >> 24) & 0xFF;

    // Bytes 218-220: nbits coefficient (3 bytes, big-endian order)
    // WM sends coefficient bytes in BE: e.g., target=0x1715D35F → sends 15 D3 5F
    // target >> 0 gives LSB first on LE platform, but WM has MSB of coefficient first
    pkt[218] = (next_bm_job->target >> 16) & 0xFF;  // coefficient MSB
    pkt[219] = (next_bm_job->target >>  8) & 0xFF;  // coefficient mid
    pkt[220] = (next_bm_job->target >>  0) & 0xFF;  // coefficient LSB

    // Bytes 221-224: Starting nonce (little-endian)
    pkt[221] = (next_bm_job->starting_nonce >>  0) & 0xFF;
    pkt[222] = (next_bm_job->starting_nonce >>  8) & 0xFF;
    pkt[223] = (next_bm_job->starting_nonce >> 16) & 0xFF;
    pkt[224] = (next_bm_job->starting_nonce >> 24) & 0xFF;

    // Byte 225: Counter byte
    pkt[225] = job_id_counter;

    // Byte 226: End marker
    pkt[226] = 0xAA;

    // Byte 227: CRC-8 over bytes 0-226
    pkt[227] = kf1950_crc8(pkt, 227, 0xFF);

    // Track active job
    if (GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id_counter] != NULL) {
        free_bm_job(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id_counter]);
    }
    GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id_counter] = next_bm_job;

    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
    GLOBAL_STATE->valid_jobs[job_id_counter] = 1;
    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

#if KF1950_DEBUG_JOBS
    ESP_LOGI(TAG, "Send Job: %02X (merkle_tail: %02X %02X %02X %02X)",
             job_id_counter,
             next_bm_job->merkle_root[0], next_bm_job->merkle_root[1],
             next_bm_job->merkle_root[2], next_bm_job->merkle_root[3]);
#endif

    SERIAL_send(pkt, KF1950_JOB_PACKET_SIZE, KF1950_DEBUG_WORK);
}

// ---------------------------------------------------------------------------
// Nonce response processing
// ---------------------------------------------------------------------------
task_result *KF1950_process_work(void *pvParameters)
{
    // KF1950 nonce response: 11 bytes, NO preamble
    // [00] [chip_addr] [04|00] [06] [job_id_echo] [nonce_3B] [byte8] [counter_echo] [CRC8]
    //
    // WM analysis (verified across 5553 nonce responses):
    //   byte[0]   = 0x00 (always)
    //   byte[1]   = chip address (0x00 for our single chip)
    //   byte[2:3] = 0x04 0x06 (standard) or 0x00 0x06 (rare variant)
    //   byte[4]   = job_id echo (matches TX byte 5)
    //   byte[5:7] = nonce bytes (3 bytes)
    //   byte[8]   = additional nonce/core byte
    //   byte[9]   = counter echo (matches TX byte 225)
    //   byte[10]  = CRC-8

    uint8_t buf[KF1950_NONCE_RESPONSE_SIZE];
    memset(&result, 0, sizeof(task_result));

    int received = SERIAL_rx(buf, KF1950_NONCE_RESPONSE_SIZE, 10000);
    if (received <= 0) {
        return NULL;
    }

    // Log every received packet for the first 30, then every 200th
    {
        static int pkt_count = 0;
        if (pkt_count < 30 || (pkt_count % 200) == 0) {
            char hex[48] = {0};
            int pos = 0;
            for (int i = 0; i < received && pos < 44; i++)
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
            ESP_LOGI(TAG, "rx[%d] %d bytes: %s", pkt_count, received, hex);
        }
        pkt_count++;
    }

    if (received != KF1950_NONCE_RESPONSE_SIZE) {
        ESP_LOGW(TAG, "Invalid response length: %d", received);
        SERIAL_clear_buffer();
        return NULL;
    }

    // Validate header bytes — accept both 04 06 (standard) and 00 06 (variant)
    if (buf[0] != 0x00 || buf[3] != 0x06 ||
        (buf[2] != 0x04 && buf[2] != 0x00)) {
        ESP_LOGW(TAG, "Invalid nonce header: %02X %02X %02X %02X", buf[0], buf[1], buf[2], buf[3]);
        SERIAL_clear_buffer();
        return NULL;
    }

    // Verify CRC-8
    uint8_t crc = kf1950_crc8(buf, 10, 0xFF);
    if (crc != buf[10]) {
        ESP_LOGW(TAG, "Nonce CRC failed: calc=%02X got=%02X", crc, buf[10]);
        SERIAL_clear_buffer();
        return NULL;
    }

    // Extract nonce (bytes 4-7, little-endian)
    // Verified by kf1950_wm_init_job.py: frame[4:8] = nonce
    uint32_t nonce = (uint32_t)buf[4] |
                     ((uint32_t)buf[5] << 8) |
                     ((uint32_t)buf[6] << 16) |
                     ((uint32_t)buf[7] << 24);

    // Job ID comes from byte 9 (counter echo, matches TX byte 225 = job_id_counter)
    // byte[4] is the first nonce byte, NOT the job_id
    uint8_t job_id = buf[9];

    uint8_t chip_addr = buf[1];

    ESP_LOGI(TAG, "Nonce: 0x%08" PRIX32 " Job: %02X Chip: %02X Counter: %02X",
             nonce, job_id, chip_addr, buf[9]);

    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    if (GLOBAL_STATE->valid_jobs[job_id] == 0) {
        ESP_LOGW(TAG, "Invalid job nonce, job_id=0x%02X", job_id);
        return NULL;
    }

    result.job_id = job_id;
    result.nonce = nonce;
    result.rolled_version = 0; // KF1950 does not support version rolling
    result.asic_nr = chip_addr;

    return &result;
}

// ---------------------------------------------------------------------------
// Baud rate control
// ---------------------------------------------------------------------------
int KF1950_set_max_baud(void)
{
    // KF1950 stays at init baud — final baud commands are skipped
    return KF1950_INIT_BAUD;
}

int KF1950_set_default_baud(void)
{
    return KF1950_INIT_BAUD;
}

// ---------------------------------------------------------------------------
// Frequency control
// ---------------------------------------------------------------------------
void KF1950_send_hash_frequency(float target_freq)
{
    // TODO: Implement PLL frequency formula for KF1950
    // For now, frequency is set during init and not changed dynamically
    ESP_LOGI(TAG, "Hash frequency request: %.0f MHz (not yet implemented)", target_freq);
}

// ---------------------------------------------------------------------------
// Version mask (KF1950 does not support version rolling)
// ---------------------------------------------------------------------------
void KF1950_set_version_mask(uint32_t version_mask)
{
    ESP_LOGI(TAG, "Version mask: 0x%08" PRIX32 " (not supported by KF1950)", version_mask);
}

// ---------------------------------------------------------------------------
// Register read
// ---------------------------------------------------------------------------
void KF1950_read_registers(void)
{
    // Status read disabled: the ASIC response lands in the nonce SERIAL_rx buffer
    // and causes process_work to drop nonces. KF1950 does not use this for temperature.
}
