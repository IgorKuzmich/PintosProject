#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;
/*ALARM CLOCK*/
static bool wake_up;
static int64_t sleep_waker_wake_up_time;
/*ALARM CLOCK*/

/*ALARM_CLOCK*/
static struct list sleeping_list;       /*list of sleeping threads*/
static struct lock sleeping_list_lock;  /*synchronization mechanism for sleeping_list*/
static struct semaphore wake_up_sleep_waker_thread; /*wakes up sleep_waker_thread*/
/*ALARM_CLOCK*/

/*FIXED POINT*/
inline int i_to_fp(int i, int q);
inline int fp_to_i_rd(int i, int q);
inline int fp_to_i_rn(int i, int q);
inline int fp_add_fp(int x, int y);
inline int fp_sub_fp(int x, int y);
inline int fp_add_i(int fp, int i, int q);
inline int fp_sub_i(int fp, int i, int q);
inline int fp_mult_fp(int x, int y, int q);
inline int fp_div_by_fp(int x, int y, int q);
inline int fp_mult_i(int fp, int i);
inline int fp_div_by_i(int fp, int i);
/*FIXED POINT*/

/*MLFSQ*/
int calc_priority(int fp_recent_cpu, int nice);
int calc_recent_cpu(int fp_load_avg, int fp_recent_cpu, int nice);
int calc_load_avg(int fp_load_avg, int ready_threads);

void calc_priority_foreach (struct thread *t, void *aux UNUSED);
void calc_recent_cpu_foreach (struct thread *t, void *aux UNUSED);

int fp_load_avg;
/*MLFSQ*/

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/*ALARM_CLOCK*/
static bool less_than_time_to_wake_up(const struct list_elem* a, const struct list_elem* b,
                                      void* aux UNUSED);
void sleep_waker_thread(void *aux UNUSED);
/*ALARM_CLOCK*/

/*ALARM_CLOCK*/
/* Compares elements based on a threads time to wake up
  used to for sleeping_list elements*/
static bool less_than_time_to_wake_up(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED)
{
  int64_t a_time_to_wake_up = (list_entry(a, struct thread, sleeping_list_elem))->time_to_wake_up;
  int64_t b_time_to_wake_up = (list_entry(b, struct thread, sleeping_list_elem))->time_to_wake_up;
  return a_time_to_wake_up < b_time_to_wake_up;
}
/*ALARM_CLOCK*/

/*ALARM_CLOCK*/
/* wakes up threads whos past the time of waking up*/
void sleep_waker_thread(void *aux UNUSED)
{
  enum intr_level old_level;
  for(;;)
  {
    //printf("------Begining sleep_waker_thread------\n");
    sema_down(&wake_up_sleep_waker_thread);
    //printf("------Got Past sema_down------\n");
    lock_acquire(&sleeping_list_lock);
    while(!list_empty(&sleeping_list))
    {
      //printf("-----Running sleep_waker_thread-----\n");
      struct thread *first_thread = list_entry(list_front(&sleeping_list), struct thread, sleeping_list_elem);
      if(first_thread->time_to_wake_up <= timer_ticks())
      {
        //printf("-----Popping Front Thread-----\n");
        list_pop_front(&sleeping_list);
        sema_up(&(first_thread->sleep));
      }
      else
      {
        enum intr_level old_level = intr_disable ();
        wake_up = true;
        sleep_waker_wake_up_time = first_thread->time_to_wake_up;
        intr_set_level (old_level);

        lock_release(&sleeping_list_lock);
        sema_down(&wake_up_sleep_waker_thread);
        lock_acquire(&sleeping_list_lock);
      }
    }
    old_level = intr_disable ();
    wake_up = false;
    intr_set_level (old_level);
    lock_release(&sleeping_list_lock);
  }
}
/*ALARM_CLOCK*/

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
  /*ALARM_CLOCK*/
  /*initializing global variables, since timer_init should only run once */
  list_init(&sleeping_list);
  lock_init(&sleeping_list_lock);
  sema_init(&wake_up_sleep_waker_thread, 0);
  wake_up = false;
  sleep_waker_wake_up_time = 0;
  /*ALARM_CLOCK*/
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  enum intr_level old_level;
  /*ALARM_CLOCK*/
  /*get start timer_ticks as soon as possible to get most accurate amount of sleep*/
  int64_t start = timer_ticks ();

  /*ASSERT (intr_get_level () == INTR_ON);
  while (timer_elapsed (start) < ticks) 
    thread_yield ();*/

  /*get current thread */
  struct thread *current_thread = thread_current();

  /*set threads wake_up time*/
  current_thread->time_to_wake_up = start + ticks;

  /*add thread to sleeping list*/
  lock_acquire(&sleeping_list_lock);
  list_insert_ordered(&sleeping_list, &(current_thread->sleeping_list_elem), less_than_time_to_wake_up, NULL);

  old_level = intr_disable ();

  sleep_waker_wake_up_time = (list_entry(list_front(&sleeping_list),struct thread, sleeping_list_elem))->time_to_wake_up;
  wake_up = true;

  intr_set_level (old_level);

  lock_release(&sleeping_list_lock);
  /* put thread to sleep */
  sema_down(&(current_thread->sleep));
  /*ALARM_CLOCK*/
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  enum intr_level old_level;
  ticks++;
  thread_tick ();
  /*ALARM_CLOCK*/
  /*wakes up sleep_waker_thread*/
  old_level = intr_disable ();
  bool yield_on_return = false;
  if(wake_up && (timer_ticks() >= sleep_waker_wake_up_time ))
  {
    wake_up = false;
    sema_up(&wake_up_sleep_waker_thread);
    yield_on_return = true;
  }

  if(yield_on_return == true)
  {
    intr_yield_on_return();
  }

  if(thread_mlfqs)
  {
    if(timer_ticks() % 4 == 0)
    {
      thread_foreach(calc_priority_foreach, NULL);
    }
    if(timer_ticks() % TIMER_FREQ == 0)
    {
      fp_load_avg = calc_load_avg(fp_load_avg, (int) ready_threads_amount());
      thread_foreach(calc_recent_cpu_foreach, NULL);
    }
  }

  intr_set_level (old_level);
  /*ALARM_CLOCK*/
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}

/*FIXED POINT*/
inline int i_to_fp(int i, int q)
{
  return i * (1 << q);
}

inline int fp_to_i_rd(int i, int q)
{
  return i / (1 << q);
}

inline int fp_to_i_rn(int i, int q)
{
  return (i >= 0) ? (i + (1 << q)/2) / (1 << q) : (i - (1 << q)/2) / (1 << q);
}

inline int fp_add_fp(int x, int y)
{
  return x + y;
}

inline int fp_sub_fp(int x, int y)
{
  return x - y;
}

inline int fp_add_i(int fp, int i, int q)
{
  return fp + i * (1 << q);
}

inline int fp_sub_i(int fp, int i, int q)
{
  return fp + i * (1 << q);
}

inline int fp_mult_fp(int x, int y, int q)
{
  return ((int64_t) x ) * y / (1 << q);
}

inline int fp_div_by_fp(int x, int y, int q)
{
  return ((int64_t) x ) * (1 << q) / y;
}

inline int fp_mult_i(int fp, int i)
{
  return fp * i;
}

inline int fp_div_by_i(int fp, int i)
{
  return fp/i;
}
/*FIXED POINT*/

/*MLFSQ*/
int calc_priority(int fp_recent_cpu, int nice)
{
  int fp_priority = i_to_fp(PRI_MAX, Q_SHIFT) - (fp_recent_cpu / 4) - (i_to_fp(nice, Q_SHIFT) * 2);
  int priority = fp_to_i_rd(fp_priority, Q_SHIFT);

  if(priority > PRI_MAX)
  {
    return PRI_MAX;
  }
  else if(priority < PRI_MIN)
  {
    return PRI_MIN;
  }
  else
  {
    return priority;
  }
}

int calc_recent_cpu(int fp_load_avg, int fp_recent_cpu, int nice)
{
  int fp_x1 = 2*fp_load_avg;
  int fp_x2 = fp_add_i(2*fp_load_avg, 1, Q_SHIFT);
  int fp_x3 = fp_div_by_fp(fp_x1, fp_x2, Q_SHIFT);
  int fp_x4 = fp_mult_fp(fp_x3, fp_recent_cpu, Q_SHIFT);
  int fp_r_cpu = fp_add_i(fp_x4, nice, Q_SHIFT);

  return fp_r_cpu;
}

int calc_load_avg(int fp_load_avg, int ready_threads)
{
  int fp_59_div_60 = i_to_fp(59, Q_SHIFT) / 60;
  int fp_1_div_60 = i_to_fp(1, Q_SHIFT) / 60;

  int fp_x1 = fp_mult_fp(fp_59_div_60, fp_load_avg, Q_SHIFT);
  int fp_x2 = fp_1_div_60 * ready_threads;

  return fp_x1 + fp_x2;
}

void calc_priority_foreach (struct thread *t, void *aux UNUSED)
{
  t->priority = calc_priority(t->fp_recent_cpu, t->nice);
}

void calc_recent_cpu_foreach (struct thread *t, void *aux UNUSED)
{
  t->fp_recent_cpu = calc_recent_cpu(fp_load_avg, t->fp_recent_cpu, t->nice);
}
/*MLFSQ*/