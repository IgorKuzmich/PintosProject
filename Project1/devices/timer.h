#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <round.h>
#include <stdint.h>

/* Number of timer interrupts per second. */
#define TIMER_FREQ 100

void timer_init (void);
void timer_calibrate (void);

int64_t timer_ticks (void);
int64_t timer_elapsed (int64_t);

/* Sleep and yield the CPU to other threads. */
void timer_sleep (int64_t ticks);
void timer_msleep (int64_t milliseconds);
void timer_usleep (int64_t microseconds);
void timer_nsleep (int64_t nanoseconds);

/* Busy waits. */
void timer_mdelay (int64_t milliseconds);
void timer_udelay (int64_t microseconds);
void timer_ndelay (int64_t nanoseconds);

void timer_print_stats (void);


/*ALARM_CLOCK*/
void sleep_waker_thread(void* aux);
/*ALARM_CLOCK*/

/*MLFSQ*/
extern int fp_load_avg;
/*MLFSQ*/

/*FIXED POINT*/
#define Q_SHIFT 14
int i_to_fp(int i, int q);
int fp_to_i_rd(int i, int q);
int fp_to_i_rn(int i, int q);
int fp_add_fp(int x, int y);
int fp_sub_fp(int x, int y);
int fp_add_i(int fp, int i, int q);
int fp_sub_i(int fp, int i, int q);
int fp_mult_fp(int x, int y, int q);
int fp_div_by_fp(int x, int y, int q);
int fp_mult_i(int fp, int i);
int fp_div_by_i(int fp, int i);
/*FIXED POINT*/

/*MLFSQ*/
int calc_priority(int fp_recent_cpu, int nice);
int calc_recent_cpu(int fp_load_avg, int fp_recent_cpu, int nice);
int calc_load_avg(int fp_load_avg, int ready_threads);
/*MLFSQ*/

#endif /* devices/timer.h */
