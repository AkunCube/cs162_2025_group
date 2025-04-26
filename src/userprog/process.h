#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/synch.h"
#include "threads/thread.h"
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
  Wait_status* wait_status;          /* Current process wait status, shared with its parent */
  struct list children;              /* Current process spawnning children */
};

void userprog_init(void);

pid_t process_execute(const char* cmd);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */
