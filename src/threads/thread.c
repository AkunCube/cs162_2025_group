#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list fifo_ready_list;

/* List of processes eligible for execution but not currently running,
   managed using a priority scheduling policy. */
static struct list prio_ready_list;

static struct list fair_ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread* idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread* initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
  void* eip;             /* Return address. */
  thread_func* function; /* Function to call. */
  void* aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4 /* # of timer ticks to give each thread. */
#define MIN_LATENCY 1024
#define MIN_QUANTA 2
static unsigned thread_ticks; /* # of timer ticks since last scheduled. */

static void init_thread(struct thread*, const char* name, int priority);
static bool is_thread(struct thread*) UNUSED;
static void* alloc_frame(struct thread*, size_t size);
static void schedule(void);
static void thread_enqueue(struct thread* t);
static tid_t allocate_tid(void);
void thread_switch_tail(struct thread* prev);

static void kernel_thread(thread_func*, void* aux);
static void idle(void* aux UNUSED);
static struct thread* running_thread(void);

static struct thread* next_thread_to_run(void);
static struct thread* thread_schedule_fifo(void);
static struct thread* thread_schedule_prio(void);
static struct thread* thread_schedule_fair(void);
static struct thread* thread_schedule_mlfqs(void);
static struct thread* thread_schedule_reserved(void);

static void update_fair_scheduler_thread_metrics(void);
static void update_fair_scheduler_time_quantums(void);
static bool fair_scheduler_less(const struct list_elem* a, const struct list_elem* b,
                                void* aux UNUSED);

#define finit() asm("finit")
#define fsave(state) asm volatile("fsave (%0)" : : "g"(&state))
#define frstor(state) asm volatile("frstor (%0)" : : "g"(&state))

/* Determines which scheduler the kernel should use.
   Controlled by the kernel command-line options
    "-sched=fifo", "-sched=prio",
    "-sched=fair". "-sched=mlfqs"
   Is equal to SCHED_FIFO by default. */
enum sched_policy active_sched_policy;

/* Selects a thread to run from the ready list according to
   some scheduling policy, and returns a pointer to it. */
typedef struct thread* scheduler_func(void);

/* Jump table for dynamically dispatching the current scheduling
   policy in use by the kernel. */
scheduler_func* scheduler_jump_table[8] = {thread_schedule_fifo,     thread_schedule_prio,
                                           thread_schedule_fair,     thread_schedule_mlfqs,
                                           thread_schedule_reserved, thread_schedule_reserved,
                                           thread_schedule_reserved, thread_schedule_reserved};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&fifo_ready_list);
  list_init(&prio_ready_list);
  list_init(&fair_ready_list);
  list_init(&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
  struct thread* t = thread_current();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pcb != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  switch (active_sched_policy) {
    case SCHED_FAIR:
      update_fair_scheduler_thread_metrics();
      break;
    default:
      /* Enforce preemption. */
      if (++thread_ticks >= TIME_SLICE)
        intr_yield_on_return();
  }
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks,
         user_ticks);
}

/* Creates a new kernel thread with name NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  if (active_sched_policy == SCHED_FAIR) {
    update_fair_scheduler_time_quantums();
  }

  /* Add to run queue. */
  thread_unblock(t);

  // If the new thread has a higher priority than the current.
  if (priority > thread_current()->effective_priority && intr_get_level() == INTR_ON) {
    // We need to yield.
    thread_yield();
  }
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Places a thread on the ready structure appropriate for the
   current active scheduling policy.
   
   This function must be called with interrupts turned off. */
static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

  if (active_sched_policy == SCHED_FIFO)
    list_push_back(&fifo_ready_list, &t->elem);
  else if (active_sched_policy == SCHED_PRIO)
    list_push_back(&prio_ready_list, &t->elem);
  else if (active_sched_policy == SCHED_FAIR)
    list_push_back(&fair_ready_list, &t->elem);
  else
    PANIC("Unimplemented scheduling policy value: %d", active_sched_policy);
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread* t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  thread_enqueue(t);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char* thread_name(void) { return thread_current()->name; }

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread* thread_current(void) {
  struct thread* t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void) {
  ASSERT(!intr_context());

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_switch_tail(). */
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
  struct thread* cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    thread_enqueue(cur);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) {
  struct thread* cur = thread_current();

  ASSERT(new_priority >= PRI_MIN && new_priority <= PRI_MAX);
  cur->priority = new_priority;
  // We need to update the effective priority.
  thread_update_effective_priority(cur);

  // Since pintos now is a preemptive kernel, we need to yield.
  if (intr_get_level() == INTR_ON) {
    thread_yield();
  }
}

/* Returns the current thread's effective_priority. */
int thread_get_priority(void) { return thread_current()->effective_priority; }

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) { /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  /* Not yet implemented. */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void* idle_started_ UNUSED) {
  struct semaphore* idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);

  finit(); /* Initialize FPU. */

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread* running_thread(void) {
  uint32_t* esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool is_thread(struct thread* t) { return t != NULL && t->magic == THREAD_MAGIC; }

/* Does basic initialization of T as a blocked thread name
   NAME. */
static void init_thread(struct thread* t, const char* name, int priority) {
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;

  strlcpy(t->name, name, sizeof(t->name));

  t->stack = (uint8_t*)t + PGSIZE;
  t->effective_priority = t->priority = priority;
  t->pcb = NULL;
  t->magic = THREAD_MAGIC;
  list_init(&t->held_locks);

  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);

  if (active_sched_policy == SCHED_FAIR) {
    t->fair_scheduler_data.vruntime = 0;
    t->fair_scheduler_data.time_quanta = 0;
    t->fair_scheduler_data.wait_ticks = 0;
  }
  intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void* alloc_frame(struct thread* t, size_t size) {
  /* Stack data is always allocated in word-size units. */
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* First-in first-out scheduler */
static struct thread* thread_schedule_fifo(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Strict priority scheduler */
static struct thread* thread_schedule_prio(void) {
  if (!list_empty(&prio_ready_list)) {
    // Find the max priority thread in the list
    struct list_elem* max_elem = list_max(&prio_ready_list, thread_priority_less, NULL);
    struct thread* t = list_entry(max_elem, struct thread, elem);
    list_remove(max_elem);
    return t;
  } else
    return idle_thread;
}

/* Fair priority scheduler */
static struct thread* thread_schedule_fair(void) {
  if (!list_empty(&fair_ready_list)) {
    // Find the max priority thread in the list
    struct list_elem* max = list_max(&fair_ready_list, fair_scheduler_less, NULL);
    struct thread* t = list_entry(max, struct thread, elem);
    list_remove(max);
    t->fair_scheduler_data.wait_ticks = 0;
    return t;
  } else
    return idle_thread;
}

/* Multi-level feedback queue scheduler */
static struct thread* thread_schedule_mlfqs(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=mlfqs\"");
}

/* Not an actual scheduling policy — placeholder for empty
 * slots in the scheduler jump table. */
static struct thread* thread_schedule_reserved(void) {
  PANIC("Invalid scheduler policy value: %d", active_sched_policy);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread* next_thread_to_run(void) {
  return (scheduler_jump_table[active_sched_policy])();
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_switch() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_switch_tail(struct thread* prev) {
  struct thread* cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Don't reset thread_ticks if nothing changed. */
  if (prev != NULL)
    thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new thread.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_switch_tail()
   has completed. */
static void schedule(void) {
  struct thread* cur = running_thread();
  struct thread* next = next_thread_to_run();
  struct thread* prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next) {
#define FPU_STATE 108
    uint8_t fpu_state[FPU_STATE];
#undef FPU_STATE
    fsave(fpu_state);
    prev = switch_threads(cur, next);
    frstor(fpu_state);
  }
  thread_switch_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

/**
 * @brief terminate the current thread and exit the process.
 * 
 * @param exit_code 
 */
void thread_terminate(int exit_code) {
  struct process* pcb = thread_current()->pcb;
  pcb->exit_code = exit_code;
  printf("%s: exit(%d)\n", pcb->process_name, exit_code);
  process_exit();
}

/**
 * @brief Compare thread priorities for sorting in a priority queue
 * 
 * This function is a comparison callback for list_sort(), 
 * used to order threads by their priority values. It enables 
 * higher-priority threads to be positioned earlier in the list.
 * 
 * @param a Pointer to the first thread's list element
 * @param b Pointer to the second thread's list element
 * @param aux Unused auxiliary data (required by list_sort API)
 * 
 * @return true if thread a has lower priority than thread b
 * @return false if thread a has higher or equal priority to thread b
 */
bool thread_priority_less(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED) {
  struct thread* t1 = list_entry(a, struct thread, elem);
  struct thread* t2 = list_entry(b, struct thread, elem);
  return t1->effective_priority < t2->effective_priority;
}

/**
 * @brief Compare function for fair scheduler priority ordering
 * 
 * Determines thread scheduling order based on dynamic priority calculation
 * that balances base priority, accumulated wait time, and virtual runtime.
 * 
 * The effective priority is computed as:
 *   dynamic_priority = base_priority + wait_ticks
 * 
 * This formula ensures:
 * 1. Higher base priority threads are favored
 * 2. Threads waiting longer are promoted to avoid starvation
 * 3. Threads that have run more recently are demoted
 * 
 * @param a List element representing the first thread
 * @param b List element representing the second thread
 * @param UNUSED Unused auxiliary data (required by list API)
 * @return true if thread a should be scheduled before thread b
 * @return false otherwise
 */
static bool fair_scheduler_less(const struct list_elem* a, const struct list_elem* b,
                                void* aux UNUSED) {
  struct thread* t1 = list_entry(a, struct thread, elem);
  struct thread* t2 = list_entry(b, struct thread, elem);
  Fair_scheduler_data_t* fsd1 = &t1->fair_scheduler_data;
  Fair_scheduler_data_t* fsd2 = &t2->fair_scheduler_data;

  // Calculate dynamic priorities
  int t1_priority = t1->effective_priority + fsd1->wait_ticks;
  int t2_priority = t2->effective_priority + fsd2->wait_ticks;

  return t1_priority < t2_priority;
}

/**
 * @brief Performs priority donation to resolve priority inversion issues in a priority inheritance system.
 * 
 * This function implements priority donation in a chain reaction: if 'target' is waiting for a lock 
 * held by another thread, this thread (and potentially subsequent threads in the waiting chain) 
 * will have their priorities temporarily raised to at least the priority of 'donor'.
 * 
 * The donation propagates along the chain of locks that 'target' is waiting for, ensuring that 
 * all threads holding resources needed by 'target' receive the necessary priority boost to prevent 
 * priority inversion. The effective priority of each thread in the chain is updated to the maximum 
 * of its current effective priority and the donor's priority.
 * 
 * @param target The thread that is waiting for a resource and initiates the priority donation.
 * @param donor  The thread whose priority is used as the donation value.
 *               The effective priority of 'target' and all threads in its waiting chain 
 *              will be raised to at least this value.
 */
void thread_donate_priority(struct thread* target, struct thread* donor) {
  ASSERT(target != NULL);
  ASSERT(donor != NULL);
  enum intr_level old_level = intr_disable();

  // We don't need to do anything if the target thread has a higher priority.
  if (target->effective_priority >= donor->effective_priority) {
    intr_set_level(old_level);
    return;
  }

  while (target->waiting_lock != NULL) {
    struct thread* holder = target->waiting_lock->holder;
    ASSERT(holder != NULL);
    ASSERT(is_thread(holder));
    target->effective_priority = MAX(target->effective_priority, donor->effective_priority);
    //! IMPORTANT: Update the waiters priority of the lock ASAP.
    target->waiting_lock->waiters_priority =
        MAX(target->waiting_lock->waiters_priority, target->effective_priority);
    target = holder;
  }
  target->effective_priority = MAX(target->effective_priority, donor->effective_priority);

  intr_set_level(old_level);
}

/**
 * @brief Update the effective priority of the current thread
 * 
 * This function recalculates and updates the effective priority of the thread
 * based on the locks it currently holds. The effective priority is defined as the
 * higher value between the thread's base priority and the highest priority among
 * all locks it holds. 
 * 
 * @param t Pointer to the thread structure whose priority is to be updated.
 *          Must be the currently executing thread (i.e., thread_current()).
 */
void thread_update_effective_priority(struct thread* t) {
  ASSERT(t != NULL);
  ASSERT(is_thread(t));
  ASSERT(t == thread_current());

  // Restore the thread's priority to the max of its held locks.
  struct list_elem* max_elem = list_max(&t->held_locks, lock_priority_less, NULL);
  int max_priority = PRI_MIN;
  if (max_elem != list_end(&t->held_locks)) {
    struct lock* l = list_entry(max_elem, struct lock, elem);
    max_priority = l->waiters_priority;
  }
  t->effective_priority = MAX(max_priority, t->priority);
}

/**
 * @brief Recalculate and update time quanta for all threads under fair scheduling policy
 * 
 * This function computes the time slice allocation for each thread based on their 
 * priority weights when using the SCHED_FAIR scheduling algorithm. 
 * 
 * The time quantum for each thread is determined by:
 *   1. Calculating its priority proportion relative to the total priority of all threads
 *   2. Applying this proportion to the MIN_LATENCY period
 *   3. Ensuring the result meets the minimum time quantum requirement (MIN_QUANTA)
 * 
 * This approach ensures higher priority threads receive larger time slices 
 * while maintaining fairness across the system. The calculation is performed 
 * atomically with interrupts disabled to prevent race conditions.
 * 
 * Preconditions:
 * - Current scheduling policy must be SCHED_FAIR
 * - Interrupts should be manageable (will be disabled temporarily)
 */
static void update_fair_scheduler_time_quantums(void) {
  ASSERT(active_sched_policy == SCHED_FAIR);
  enum intr_level old_level = intr_disable();
  int total_priority = 0;

  // Step 1: Aggregate total priority across all threads.
  for (struct list_elem* e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    total_priority += t->priority;
  }

  // Step 2: Calculate and assign time quanta based on priority ratios.
  for (struct list_elem* e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    float proportion = ((float)t->priority) / ((float)total_priority);
    t->fair_scheduler_data.time_quanta = MAX(MIN_QUANTA, MIN_LATENCY * proportion);
  }

  intr_set_level(old_level);
}

/**
 * @brief Update fair scheduler metrics and manage time quantum for the current thread
 * 
 * This function performs per-tick accounting for the fair scheduling algorithm:
 * 1. Increments the current thread's virtual runtime
 * 2. Updates wait ticks for all ready-to-run threads except the current one
 * 3. Checks if the current thread has exhausted its time quantum
 * 4. Recalculates and resets the time quantum if exhausted.
 * 
 * The time quantum is dynamically adjusted based on the thread's priority relative
 * to the total system priority, ensuring fairness while maintaining responsiveness.
 * 
 * Preconditions:
 * - Current scheduling policy must be SCHED_FAIR
 * - Function should be called once per timer tick
 */
static void update_fair_scheduler_thread_metrics(void) {
  ASSERT(active_sched_policy == SCHED_FAIR);
  struct thread* cur = thread_current();

  // Update current thread's virtual runtime
  Fair_scheduler_data_t* fsd = &cur->fair_scheduler_data;
  ++fsd->vruntime;

  // Track wait time for other ready threads
  for (struct list_elem* e = list_begin(&fair_ready_list); e != list_end(&fair_ready_list);
       e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, elem);
    Fair_scheduler_data_t* fsd = &t->fair_scheduler_data;
    if (t != cur) {
      ++fsd->wait_ticks;
    }
  }

  // Check time quantum expiration and trigger rescheduling if needed
  if (++thread_ticks >= fsd->time_quanta) {
    intr_yield_on_return();

    // Recalculate time quantum based on priority weights
    int total_priority = 0;
    for (struct list_elem* e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
      struct thread* t = list_entry(e, struct thread, allelem);
      total_priority += t->priority;
    }
    float proportion = ((float)cur->priority) / ((float)total_priority);
    fsd->time_quanta = MAX(MIN_QUANTA, MIN_LATENCY * proportion);
  }
}
