#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include <stdint.h>

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

#define MAX_OPEN_FILE 128
#define MAX_SYNC 128

/**
 * @brief Manages the thread collection for a process.
 * 
 * This structure maintains the list of all threads belonging to a process.
 * It includes an internal lock to ensure thread-safe operations when
 * modifying the thread list (e.g., adding/removing threads).
 * 
 * Key features:
 * - Thread safety through internal locking mechanism
 * - Linked list implementation for dynamic thread management
 */
typedef struct {
  struct lock lock;    /* Protects the list of threads */
  struct list threads; /* List of threads in this process */
} Thread_list;

/**
 * @brief Represents a thread stack allocation within the kernel.
 * 
 * This structure tracks the memory boundaries and list linkage of a 
 * contiguous stack page region. Each Stack_slot corresponds to a 
 * virtual stack allocation used by a thread during execution.
 * 
 * @note The stack grows downward from stackpg_top to stackpg_bottom.
 */
typedef struct {
  void* stackpg_top;     /* Top of the stack page (highest address) */
  void* stackpg_bottom;  /* Bottom of the stack page (lowest address) */
  bool is_mapped;        /* Indicates if the stack page is mapped */
  struct list_elem elem; /* List element for linking stack slots */
} Stack_slot;

/**
 * @brief Manages the allocation and deallocation of thread stacks.
 * 
 * This structure centralizes the management of user stack pages. It 
 * maintains a pool of free stack slots using a linked list, ensuring 
 * efficient allocation and reuse of stack memory. The stack manager 
 * handles contiguous memory regions and protects shared state with a lock.
 */
typedef struct {
  struct lock lock;        /* Protects the Stack_manager */
  struct list free_stacks; /* List of free stack slots */
  void* stack_base;        /* Base address of the stack region */
  int alloc_pgcnt;         /* Number of stack pages allocated */
} Stack_manager;

typedef struct {
  /* Child pid, parent process used to search child */
  pid_t pid;
  /* Protects ref_cnt. */
  struct lock lock;
  /*  2 : child and parent both alive,
      1 : either child or parent alive,
      0 : child and parent both dead. */
  int ref_cnt;
  /* Child exit code, if dead. */
  int exit_code;
  /* 1 : child dead, 0 : child alive. */
  struct semaphore dead;
  struct list_elem elem;
} Wait_status;

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  pid_t pid;                         /* Process id*/
  pid_t ppid;                        /* Parent process id*/
  int exit_code;                     /* Process exit code */
  uint32_t* pagedir;                 /* Page directory. */
  char process_name[16];             /* Name of the main thread */
  struct thread* main_thread;        /* Pointer to main thread */
  struct file* ofile[MAX_OPEN_FILE]; /* Open-file table */
  struct file* elf_file;             /* Current executable file */
  Wait_status* wait_status;          /* Current process wait status, shared with its parent */
  struct list children;              /* Current process spawnning children */
  Thread_list threads;               /* List of threads in this process */
  Stack_manager stack_manager;       /* Stack manager for this process */
  struct lock* user_locks[MAX_SYNC]; /* User locks for this process */
  struct semaphore* user_semaphores[MAX_SYNC]; /* User semaphores for this process */
  bool set_exit_code;                          /* If true, the process has set an exit code */
};

void userprog_init(void);

pid_t process_execute(const char* cmd);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);
void process_exit_with_status(int exit_code);
pid_t process_fork(const struct intr_frame* if_);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, const void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */
