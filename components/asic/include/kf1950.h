#ifndef KF1950_H_
#define KF1950_H_

#include "asic_common.h"
#include "asic_job.h"

typedef struct GlobalState GlobalState;

#define KF1950_SERIALTX_DEBUG false
#define KF1950_SERIALRX_DEBUG false
#define KF1950_DEBUG_WORK false //causes insane amount of debug output
#define KF1950_DEBUG_JOBS false //causes insane amount of debug output

// One fresh job per chip every 42 ms. The KF1950 restarts a job whenever it is
// re-sent, so jobs must be paced rather than streamed.
#define KF1950_JOB_INTERVAL_MS 42

uint8_t KF1950_init(GlobalState * GLOBAL_STATE);
void KF1950_send_work(GlobalState * GLOBAL_STATE, asic_job_t * next_job);
void KF1950_set_version_mask(uint32_t version_mask);
int KF1950_set_max_baud(void);
float KF1950_send_hash_frequency(float frequency);
task_result * KF1950_process_work(GlobalState * GLOBAL_STATE);
void KF1950_read_registers(void);
void KF1950_set_difficulty(uint8_t difficulty_power);

// Die temperature in deg C (register 0x24A), or -1 if not read in the last 2 minutes.
float KF1950_get_die_temp(void);

#endif /* KF1950_H_ */
