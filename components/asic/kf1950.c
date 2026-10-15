#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kf1950.h"
#include "bm_job_midstate.h"
#include "global_state.h"
#include "mining.h"
#include "serial.h"
#include "utils.h"

static const char * TAG = "kf1950";

// The KF1950 (WhatsMiner M30S hashing chip) has no discrete opcodes: bytes [2][3]
// of a frame are a 12-bit register address plus an access nibble (4 = write,
// 0 = read). Every host frame is FF FF [payload] [CRC-8], or [addr] 00 [payload]
// [CRC-8] when addressed to one chip. The init and re-arm sequences below are
// reproduced byte-for-byte from the stock WhatsMiner controller.

#define KF1950_CHIP_ID              0x1950
#define KF1950_INIT_BAUD            375000
#define KF1950_MAX_BAUD             2000000
#define KF1950_BAUD_MODE_SEL        0x02
#define KF1950_BAUD_DIV_2M          0x8009
#define KF1950_CHIP_ID_RESPONSE_LEN 11
#define KF1950_PLL_N_INIT           0x80   // bring-up PLL feedback divider
#define KF1950_REF_CLK_MHZ          24
#define KF1950_END                  0xAA
#define KF1950_CRC_INIT             0xFF

#define KF1950_BRINGUP_FREQ_MHZ     198    // first re-arm runs here; the next one moves to target
#define KF1950_REARM_PERIOD_MS      2500   // the stock controller re-arms every ~2.7 s
#define KF1950_JOB_PRELOAD_COUNT    10     // jobs queued while the chip is halted
#define KF1950_DIE_TEMP_PERIOD_MS   30000
#define KF1950_DIE_TEMP_STALE_MS    120000

// The chip has no hash counter registers, so hashrate is estimated from nonce counts.
// The ticket mask written in rearm_cycle (00 00 00 C0) makes the chip return every
// nonce of difficulty >= 4, i.e. one nonce per 4 x 2^32 hashes on average.
#define KF1950_NONCE_DIFF           4
#define KF1950_HASH_COUNT_PERIOD_MS 10000

// Opcodes (first payload byte)
#define CMD_READ_ID     0x10
#define CMD_STATUS      0x11
#define CMD_CONFIG_A    0x12
#define CMD_BCAST_REG   0x00
#define CMD_CONFIG_B    0x07
#define CMD_PLL         0x03
#define CMD_PLL_FINAL   0x20
#define CMD_CORE_EN     0x04
#define CMD_NONCE_RANGE 0x80
#define CMD_HASH_MODE   0x8D
#define CMD_POST_02     0x02
#define CMD_POST_23     0x23
#define CMD_POST_24     0x24
#define CMD_MAINT       0x05
#define CMD_TRIG_POST   0x06
#define CMD_BAUD        0x01
#define CMD_X81_JOB     0x81

// Register 0x051 values written around the core-enable window (05 14 04 ...)
#define MAINT_SET_TARGET 0xFF, 0xFF, 0xFF, 0xFF
#define MAINT_HALT       0x00, 0x00, 0x00, 0xFF
#define MAINT_RESUME     0x00, 0x00, 0xF8, 0xFF
#define MAINT_HASH       0x00, 0x00, 0x00, 0xC0

// X81 job: FF FF 81 04 CE + 6 midstates + nbits + ntime + merkle tail + id + AA + CRC
#define KF1950_X81_MIDSTATES   6
#define KF1950_X81_PAYLOAD_LEN 0xCE

typedef struct __attribute__((__packed__))
{
    uint8_t preamble[2];
    uint8_t cmd;
    uint8_t reg;
    uint8_t len;
    uint8_t midstates[KF1950_X81_MIDSTATES][32]; // word-reversed; slot k = version rolled k times
    uint8_t nbits[4];                            // header bytes 72..75
    uint8_t ntime[4];                            // header bytes 68..71
    uint8_t merkle_tail[4];                      // header bytes 64..67
    uint8_t job_id;
    uint8_t end_marker;
    uint8_t crc;
} kf1950_x81_job_t;

_Static_assert(sizeof(kf1950_x81_job_t) == 212, "kf1950_x81_job_t size mismatch");

// Result frame: [sweep] 00 04 06 [nonce x4] [midstate field] [job id] [CRC]
// Midstate slot = 5 - midstate field. Job ids >= 0xC0 are legacy-job echoes, not nonces.
typedef struct __attribute__((__packed__))
{
    uint8_t sweep;
    uint8_t zero;
    uint8_t type;
    uint8_t len;
    uint8_t nonce[4];
    uint8_t midstate_idx;
    uint8_t job_id;
    uint8_t crc;
} kf1950_result_t;

_Static_assert(sizeof(kf1950_result_t) == 11, "kf1950_result_t size mismatch");

#define KF1950_ECHO_ID_BASE 0xC0

// Core-enable sweep: index 0x01-0x70, value-outer / index-inner, as the stock
// controller sends it (1,008 frames).
static const uint8_t CE_VALS[]    = {0x31, 0x21, 0x11, 0x91, 0xA1, 0xB1};
static const uint8_t CE_REPEATS[] = {   1,    1,    1,    2,    2,    2};
#define CE_INDEX_FIRST 0x01
#define CE_INDEX_LAST  0x70

static task_result result;
static uint8_t job_counter;
static uint8_t chip_addr;
static float target_freq = 400.0f;
static uint32_t last_rearm_ms;
static kf1950_x81_job_t job_cache;
static bool job_cached;
static SemaphoreHandle_t tx_lock;

static uint32_t hash_count;      // in units of 2^32 hashes
static uint32_t hash_count_ms;

static float die_temp_c = -1.0f;
static uint32_t die_temp_ms;
static uint32_t die_try_ms;

static uint8_t crc8(const uint8_t * data, size_t len)
{
    uint8_t crc = KF1950_CRC_INIT;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
    }
    return crc;
}

static inline uint32_t now_ms(void)
{
    return (uint32_t) pdTICKS_TO_MS(xTaskGetTickCount());
}

// The result task reads registers (die temperature) while the job task transmits;
// hold the lock for a whole frame, or across a read and its reply.
static inline void lock(void)   { xSemaphoreTakeRecursive(tx_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGiveRecursive(tx_lock); }

static void send_raw(uint8_t * data, int len, bool debug)
{
    lock();
    SERIAL_send(data, len, debug);
    unlock();
}

static void send_frame(uint8_t first, uint8_t second, const uint8_t * payload, size_t len)
{
    uint8_t buf[len + 3];
    buf[0] = first;
    buf[1] = second;
    memcpy(buf + 2, payload, len);
    buf[len + 2] = crc8(buf, len + 2);
    send_raw(buf, len + 3, KF1950_SERIALTX_DEBUG);
}

#define SEND(...) do { \
        const uint8_t _p[] = {__VA_ARGS__}; \
        send_frame(0xFF, 0xFF, _p, sizeof(_p)); \
    } while (0)

#define SEND_WAIT(ms, ...) do { SEND(__VA_ARGS__); vTaskDelay(pdMS_TO_TICKS(ms)); } while (0)

#define SEND_TO(addr, ...) do { \
        const uint8_t _p[] = {__VA_ARGS__}; \
        send_frame((addr), 0x00, _p, sizeof(_p)); \
    } while (0)

static void read_status(const char * stage, uint16_t timeout_ms)
{
    uint8_t buf[16];
    SERIAL_clear_buffer();
    SEND(CMD_STATUS, 0x00, 0x01);
    int n = SERIAL_rx(buf, sizeof(buf), timeout_ms);
    if (n >= 6) {
        ESP_LOGD(TAG, "Status (%s): %02X %02X %02X %02X %02X %02X", stage, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    } else {
        ESP_LOGW(TAG, "Status (%s): no response", stage);
    }
}

// Reads `len` bytes starting at register `reg`. Returns len, or -1 if no CRC-valid reply.
static int read_register(uint16_t reg, uint8_t len, uint8_t * out)
{
    uint8_t buf[64];
    const int rec = 5 + len;

    lock();
    SERIAL_clear_buffer();
    SEND((uint8_t) (reg >> 4), (uint8_t) ((reg & 0x0F) << 4), len);
    int n = SERIAL_rx(buf, sizeof(buf), 300);
    unlock();

    for (int i = 0; i + rec <= n; i++) {
        bool addressed = buf[i + 2] == 0x00 && buf[i + 3] == len;
        bool broadcast = buf[i] == 0xFF && buf[i + 1] == 0xFF && buf[i + 2] == 0x00 && buf[i + 3] == len;
        if ((addressed || broadcast) && crc8(&buf[i], rec - 1) == buf[i + rec - 1]) {
            memcpy(out, &buf[i + 4], len);
            return len;
        }
    }
    return -1;
}

// PLL: ref 24 MHz, VCO = fbdiv * 6 must stay within ~600-1200 MHz.
static uint8_t pll_post_div(float f)
{
    if (f < 125.0f) return 3;
    if (f < 300.0f) return 2;
    if (f < 600.0f) return 1;
    return 0;
}

static uint16_t pll_fbdiv(float f, uint8_t pd)
{
    return (uint16_t) ((float) (1u << pd) * f * 4.0f / KF1950_REF_CLK_MHZ + 0.5f);
}

static void send_pll(float freq)
{
    uint8_t pd = pll_post_div(freq);
    uint16_t fb = pll_fbdiv(freq, pd);
    SEND(CMD_PLL, 0x14, 0x06, fb >> 8, fb & 0xFF, 0x04, pd, 0x03, KF1950_END);
    SEND_TO(chip_addr, CMD_PLL, 0x14, 0x06, fb >> 8, fb & 0xFF, 0x04, pd, 0x03, KF1950_END);
}

static void core_enable_sweep(void)
{
    for (int v = 0; v < (int) sizeof(CE_VALS); v++)
        for (int r = 0; r < CE_REPEATS[v]; r++)
            for (uint8_t nn = CE_INDEX_FIRST; nn <= CE_INDEX_LAST; nn++)
                SEND(CMD_CORE_EN, 0x04, 0x04, nn, 0x3F, CE_VALS[v], KF1950_END);
}

// Commit pass: B1 twice over the 63 indices 01-0F, 21-30, 41-50, 61-70.
static void core_enable_commit(void)
{
    for (int r = 0; r < 2; r++)
        for (uint8_t nn = 0x01; nn <= 0x70; nn++) {
            if (nn == 0x10 || (nn > 0x10 && nn < 0x21) || (nn > 0x30 && nn < 0x41) || (nn > 0x50 && nn < 0x61))
                continue;
            SEND(CMD_CORE_EN, 0x04, 0x04, nn, 0x3F, 0xB1, KF1950_END);
        }
}

static void send_nonce_config(void)
{
    SEND(CMD_NONCE_RANGE, 0x24, 0x02, 0x00, 0x00);
    SEND(CMD_NONCE_RANGE, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x20, 0x00, 0x40, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00);
    SEND(CMD_HASH_MODE, 0xD4, 0x01, KF1950_END);
}

// The chip's enabled-core state decays within seconds unless this cycle is repeated.
// It halts job intake, queues work, re-applies the PLL and core enable, then resumes.
static void rearm_cycle(float freq)
{
    SEND(CMD_MAINT, 0x14, 0x04, MAINT_SET_TARGET);
    send_nonce_config();
    SEND_TO(chip_addr, CMD_NONCE_RANGE, 0x04, 0x02, 0x00, 0x00); // nonce-space base, reg 0x800

    SEND(CMD_MAINT, 0x14, 0x04, MAINT_HALT);
    if (job_cached) {
        for (int i = 0; i < KF1950_JOB_PRELOAD_COUNT; i++)
            send_raw((uint8_t *) &job_cache, sizeof(job_cache), false);
    }
    SEND(CMD_MAINT, 0x14, 0x04, MAINT_SET_TARGET);

    send_pll(freq);
    core_enable_sweep();

    // Latch pair writes reg 0x042 (the core-enable value byte); restore it to B1 after.
    SEND(CMD_CORE_EN, 0x24, 0x01, 0x00);
    SEND(CMD_CORE_EN, 0x24, 0x01, 0x01);
    SEND(CMD_CORE_EN, 0x24, 0x01, 0xB1);

    SEND(CMD_MAINT, 0x14, 0x04, MAINT_RESUME);
    SEND(CMD_MAINT, 0x14, 0x04, MAINT_HASH);
}

static int count_chips(uint16_t expected)
{
    uint8_t buf[64];
    SEND(CMD_READ_ID, 0x00, 0x06);
    int total = SERIAL_rx(buf, sizeof(buf), 2000);

    int count = 0;
    for (int offset = 0; offset + KF1950_CHIP_ID_RESPONSE_LEN <= total;) {
        uint8_t * r = buf + offset;
        if (crc8(r, KF1950_CHIP_ID_RESPONSE_LEN - 1) != r[KF1950_CHIP_ID_RESPONSE_LEN - 1]) {
            offset++;
            continue;
        }
        count++;
        offset += KF1950_CHIP_ID_RESPONSE_LEN;
    }

    if (count != expected) {
        ESP_LOGW(TAG, "%d chip(s) detected, expected %d", count, expected);
    }
    return count;
}

uint8_t KF1950_init(GlobalState * GLOBAL_STATE)
{
    uint16_t asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;

    if (!tx_lock) tx_lock = xSemaphoreCreateRecursiveMutex();

    float frequency = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
    if (frequency > 0) target_freq = frequency;

    SERIAL_set_baud(KF1950_INIT_BAUD);
    vTaskDelay(pdMS_TO_TICKS(500));
    SERIAL_clear_buffer();

    int chip_count = 0;
    for (int attempt = 0; attempt < 3 && chip_count == 0; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            SERIAL_clear_buffer();
        }
        chip_count = count_chips(asic_count);
    }
    if (chip_count == 0) {
        ESP_LOGE(TAG, "No KF1950 chips detected");
        return 0;
    }

    SEND_WAIT(50, CMD_CONFIG_A, 0x34, 0x02, 0x13, 0xF4);
    SEND_WAIT(50, CMD_CONFIG_B, 0x04, 0x02, 0x15, 0x15);
    SEND_WAIT(50, CMD_BCAST_REG, 0x04, 0x01, KF1950_END);

    // Address assignment: broadcast propose (reg 0x001 = index), then addressed confirm.
    for (uint8_t idx = 0; idx < asic_count; idx++) {
        SEND(CMD_BCAST_REG, 0x14, 0x02, idx, 0x00);
        esp_rom_delay_us(392);
        SEND_TO(idx, 0x02, 0x04, 0x01, 0x30);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    SERIAL_clear_buffer();

    SEND_WAIT(50, CMD_CONFIG_B, 0x04, 0x02, 0x35, 0x35);

    for (uint8_t domain = 0; domain < 4; domain++) {
        SEND_WAIT(20, CMD_PLL, 0x04, 0x01, 0x05);
        SEND_WAIT(20, CMD_PLL, 0x14, 0x06, 0x00, KF1950_PLL_N_INIT, 0x04, 0x05, domain, KF1950_END);
        SEND_WAIT(20, CMD_PLL, 0x04, 0x01, 0x04);
        SEND_WAIT(20, CMD_PLL, 0x04, 0x01, 0x04);
        SEND_WAIT(20, CMD_PLL, 0x14, 0x06, 0x00, KF1950_PLL_N_INIT, 0x04, 0x05, domain, KF1950_END);
    }

    SEND_WAIT(50, CMD_NONCE_RANGE, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x20, 0x00, 0x40, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00);
    SEND_WAIT(50, CMD_HASH_MODE, 0xD4, 0x01, KF1950_END);
    core_enable_sweep();
    vTaskDelay(pdMS_TO_TICKS(10));
    read_status("core enable", 500);

    SEND_WAIT(20, CMD_POST_02, 0x14, 0x01, 0x3A);
    SEND_WAIT(20, 0x22, 0x04, 0x03, 0x13, 0x1E, KF1950_END);
    SEND_WAIT(20, CMD_POST_23, 0x04, 0x06, 0x26, 0x1F, 0x30, 0x80, 0x00, KF1950_END);
    SEND_WAIT(20, CMD_POST_24, 0x04, 0x07, 0x25, 0xFE, 0x50, 0x00, 0x60, 0x00, 0x00);
    SEND_WAIT(20, CMD_POST_24, 0x94, 0x01, KF1950_END);
    read_status("post config", 1000);

    SEND_WAIT(50, CMD_MAINT, 0x14, 0x04, MAINT_SET_TARGET);
    SEND_WAIT(50, CMD_MAINT, 0x04, 0x01, 0x83);
    SEND_WAIT(50, CMD_TRIG_POST, 0x04, 0x01, 0x11);

    SEND_WAIT(20, CMD_PLL, 0x04, 0x01, 0x04);
    SEND_WAIT(20, CMD_PLL, 0x14, 0x06, 0x00, KF1950_PLL_N_INIT, 0x04, 0x02, 0x03, KF1950_END);
    core_enable_commit();
    vTaskDelay(pdMS_TO_TICKS(10));

    SEND_WAIT(20, CMD_PLL, 0x04, 0x01, 0x04);
    SEND_WAIT(20, CMD_PLL, 0x14, 0x06, 0x00, KF1950_PLL_N_INIT, 0x04, 0x04, 0x00, KF1950_END);
    SEND_WAIT(20, CMD_PLL_FINAL, 0x04, 0x01, 0x04);
    SEND_WAIT(20, CMD_PLL_FINAL, 0x14, 0x06, 0x00, KF1950_PLL_N_INIT, 0x04, 0x04, 0x0F, KF1950_END);
    read_status("final pll", 500);

    SERIAL_clear_buffer();
    vTaskDelay(pdMS_TO_TICKS(500));

    // The PLL is applied at the target frequency by the first re-arm cycle.
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency = target_freq;

    return chip_count;
}

int KF1950_set_max_baud(void)
{
    ESP_LOGI(TAG, "Setting max baud of %d", KF1950_MAX_BAUD);

    SEND(CMD_BAUD, 0x44, 0x02, KF1950_BAUD_MODE_SEL, 0x00);
    SEND(CMD_BAUD, 0x04, 0x04, 0x01, KF1950_BAUD_DIV_2M >> 8, KF1950_BAUD_DIV_2M & 0xFF, KF1950_END);

    SERIAL_set_baud(KF1950_MAX_BAUD);
    vTaskDelay(pdMS_TO_TICKS(2));
    SERIAL_clear_buffer();

    // Learn the chip's bus address from its enumeration reply: [addr] .. .. 02 19 50 [CRC]
    chip_addr = 0x00;
    SEND(CMD_READ_ID, 0x00, 0x02);
    uint8_t e[64];
    int n = SERIAL_rx(e, sizeof(e), 200);
    for (int i = 0; i + 7 <= n; i++) {
        if (e[i + 3] == 0x02 && e[i + 4] == (KF1950_CHIP_ID >> 8) && e[i + 5] == (KF1950_CHIP_ID & 0xFF) &&
            crc8(&e[i], 6) == e[i + 6]) {
            chip_addr = (e[i] == 0x00 && e[i + 1] != 0x00) ? e[i + 1] : e[i];
            break;
        }
    }

    SEND_TO(chip_addr, CMD_NONCE_RANGE, 0x04, 0x02, 0x00, 0x00);
    vTaskDelay(pdMS_TO_TICKS(2));
    send_nonce_config();
    vTaskDelay(pdMS_TO_TICKS(5));
    SERIAL_clear_buffer();

    // The stock controller repeats the full core-enable bring-up at the fast baud.
    core_enable_sweep();
    core_enable_sweep();

    last_rearm_ms = now_ms();
    rearm_cycle(KF1950_BRINGUP_FREQ_MHZ);
    read_status("post enable", 150);
    SERIAL_clear_buffer();

    return KF1950_MAX_BAUD;
}

static uint32_t version_for_slot(const asic_job_t * job, uint8_t slot)
{
    uint32_t version = job->version;
    for (uint8_t i = 0; i < slot; i++)
        version = increment_bitmask(version, job->version_mask);
    return version;
}

static void build_x81_job(kf1950_x81_job_t * packet, const asic_job_t * job, uint8_t job_id)
{
    uint8_t header[80];
    uint8_t midstate[32];

    packet->preamble[0] = 0xFF;
    packet->preamble[1] = 0xFF;
    packet->cmd = CMD_X81_JOB;
    packet->reg = 0x04;
    packet->len = KF1950_X81_PAYLOAD_LEN;

    for (uint8_t slot = 0; slot < KF1950_X81_MIDSTATES; slot++) {
        asic_job_header(job, 0, version_for_slot(job, slot), header);
        midstate_sha256_bin(header, 64, midstate);
        reverse_32bit_words(midstate, packet->midstates[slot]);
    }

    memcpy(packet->merkle_tail, header + 64, 4);
    memcpy(packet->ntime, header + 68, 4);
    memcpy(packet->nbits, header + 72, 4);
    packet->job_id = job_id;
    packet->end_marker = KF1950_END;
    packet->crc = crc8((uint8_t *) packet, sizeof(*packet) - 1);
}

void KF1950_send_work(GlobalState * GLOBAL_STATE, asic_job_t * next_job)
{
    uint32_t now = now_ms();
    if (now - last_rearm_ms >= KF1950_REARM_PERIOD_MS) {
        last_rearm_ms = now;
        rearm_cycle(target_freq);
    }

    job_counter = (job_counter + 1) % MAX_ASIC_JOBS;

    kf1950_x81_job_t packet;
    build_x81_job(&packet, next_job, job_counter);

    pthread_mutex_lock(&GLOBAL_STATE->ASIC_TASK_MODULE.valid_jobs_lock);
    if (GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_counter] != NULL) {
        free(GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_counter]);
    }
    GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_counter] = next_job;
    GLOBAL_STATE->ASIC_TASK_MODULE.valid_jobs[job_counter] = 1;
    pthread_mutex_unlock(&GLOBAL_STATE->ASIC_TASK_MODULE.valid_jobs_lock);

    #if KF1950_DEBUG_JOBS
    ESP_LOGI(TAG, "Send Job: %02X", job_counter);
    #endif

    send_raw((uint8_t *) &packet, sizeof(packet), KF1950_DEBUG_WORK);

    job_cache = packet;
    job_cached = true;
}

// Called from the result task, which owns UART RX, so the read cannot steal a
// result frame from another reader.
static void sample_die_temp(void)
{
    uint32_t now = now_ms();
    if (die_try_ms != 0 && now - die_try_ms < KF1950_DIE_TEMP_PERIOD_MS) return;
    die_try_ms = now ? now : 1;

    uint8_t b[2];
    if (read_register(0x24A, 2, b) == 2 && b[0] == 0x01) {
        die_temp_c = b[1];
        die_temp_ms = now;
    }
}

float KF1950_get_die_temp(void)
{
    if (die_temp_c < 0.0f || now_ms() - die_temp_ms > KF1950_DIE_TEMP_STALE_MS) return -1.0f;
    return die_temp_c;
}

task_result * KF1950_process_work(GlobalState * GLOBAL_STATE)
{
    kf1950_result_t rx;

    memset(&result, 0, sizeof(task_result));

    sample_die_temp();

    // Report the nonce-derived counter as the chip's total hash counter.
    uint32_t now = now_ms();
    if (now - hash_count_ms >= KF1950_HASH_COUNT_PERIOD_MS) {
        hash_count_ms = now;
        result.register_type = REGISTER_TOTAL_COUNT;
        result.asic_nr = 0;
        result.value = hash_count;
        result.timestamp_us = esp_timer_get_time();
        return &result;
    }

    int received = SERIAL_rx((uint8_t *) &rx, sizeof(rx), 2000);
    result.timestamp_us = esp_timer_get_time();
    if (received <= 0) {
        return NULL;
    }

    if (received != sizeof(rx) || rx.zero != 0x00 || rx.type != 0x04 || rx.len != 0x06 || crc8((uint8_t *) &rx, sizeof(rx) - 1) != rx.crc) {
        ESP_LOGW(TAG, "Invalid result frame");
        SERIAL_clear_buffer();
        return NULL;
    }

    if (rx.job_id >= KF1950_ECHO_ID_BASE) {
        return NULL;
    }

    uint8_t job_id = rx.job_id;
    pthread_mutex_lock(&GLOBAL_STATE->ASIC_TASK_MODULE.valid_jobs_lock);
    if (job_id >= MAX_ASIC_JOBS || GLOBAL_STATE->ASIC_TASK_MODULE.valid_jobs[job_id] == 0 ||
        GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id] == NULL) {
        pthread_mutex_unlock(&GLOBAL_STATE->ASIC_TASK_MODULE.valid_jobs_lock);
        ESP_LOGW(TAG, "Invalid job nonce found, id=%d", job_id);
        return NULL;
    }
    result.job = *GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id];
    pthread_mutex_unlock(&GLOBAL_STATE->ASIC_TASK_MODULE.valid_jobs_lock);

    hash_count += KF1950_NONCE_DIFF;

    uint8_t slot = rx.midstate_idx <= 5 ? 5 - rx.midstate_idx : 0;

    result.nonce = (uint32_t) rx.nonce[0] | ((uint32_t) rx.nonce[1] << 8) |
                   ((uint32_t) rx.nonce[2] << 16) | ((uint32_t) rx.nonce[3] << 24);
    result.rolled_version = version_for_slot(&result.job, slot);
    result.asic_nr = rx.sweep;

    return &result;
}

float KF1950_send_hash_frequency(float frequency)
{
    // Applied by the next re-arm cycle (within KF1950_REARM_PERIOD_MS). Do not step the
    // PLL here: intermediate frequency steps stop the chip returning nonces.
    target_freq = frequency;
    uint8_t pd = pll_post_div(frequency);
    ESP_LOGI(TAG, "Setting Frequency to %g MHz (pd=%u fbdiv=%u)", frequency, pd, pll_fbdiv(frequency, pd));
    return frequency;
}

void KF1950_set_version_mask(uint32_t version_mask)
{
    // Version rolling is done in software across the six X81 midstates.
}

void KF1950_read_registers(void)
{
    // No hashrate or domain counter registers are known for this chip yet.
}

void KF1950_set_difficulty(uint8_t difficulty_power)
{
    // Ticket mask (reg 0x051) is fixed by the re-arm cycle; results are filtered in software.
}
