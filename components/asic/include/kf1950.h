#ifndef KF1950_H_
#define KF1950_H_

#include "common.h"
#include "mining.h"

#define KF1950_SERIALTX_DEBUG true
#define KF1950_SERIALRX_DEBUG true
#define KF1950_DEBUG_WORK true
#define KF1950_DEBUG_JOBS true

#define KF1950_CHIP_ID 0x1950
#define KF1950_INIT_BAUD 363636
#define KF1950_JOB_PACKET_SIZE 228
#define KF1950_NONCE_RESPONSE_SIZE 11
#define KF1950_CHIP_ID_RESPONSE_LENGTH 11

uint8_t KF1950_init(float frequency, uint16_t asic_count, uint16_t difficulty);
void KF1950_send_work(void * GLOBAL_STATE, bm_job * next_bm_job);
void KF1950_set_version_mask(uint32_t version_mask);
int KF1950_set_max_baud(void);
int KF1950_set_default_baud(void);
void KF1950_send_hash_frequency(float frequency);
task_result * KF1950_process_work(void * GLOBAL_STATE);
void KF1950_read_registers(void);

#endif /* KF1950_H_ */
