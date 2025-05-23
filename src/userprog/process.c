#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "stddef.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char** user_argv, int argc, void (**eip)(void), void** esp);
bool setup_thread(void** esp, Join_status* js);
static void handle_exit_wait_status(struct thread* cur, int exit_code);
static void handle_exit_close_files(struct thread* cur);
static Wait_status* new_and_init_wait_status(pid_t pid);
static Join_status* new_and_init_join_status(tid_t tid);
static void init_stack_manager(Stack_manager* sm);
static bool expand_stack_pool(Stack_manager* sm, int new_size);
static Stack_slot* process_allocate_stack(Stack_manager* sm);
static void perform_fork_operations(void* args);
static int parse_user_command(char* user_cmd, const char* user_argv[], const int max_args);
static void init_process_descriptors(struct process* pcb);

#define ALIGN_MASK(type) (~(sizeof(type) - 1))

typedef struct {
  char* command;
  struct process* parent;
  struct semaphore pexec_sema; // Parent waiting for child loading.
  bool load_success;
#define MAX_ARGS 64
  const char* user_argv[MAX_ARGS];
  int argc;
} start_process_args;

typedef struct {
  stub_fun sf;
  pthread_fun tf;
  const void* arg;
  struct process* pcb;
  struct semaphore pexec_sema;
  bool success;
} start_pthread_args;

typedef struct {
  const struct intr_frame* if_;
  struct process* parent;
  struct semaphore pexec_sema; // Parent waiting for child loading.
  bool load_success;
} fork_process_args;

static void init_start_process_args(start_process_args* args, char* command) {
  args->command = command;
  // Parse the user command to get the executable name and arguments
  args->argc = parse_user_command(command, args->user_argv, MAX_ARGS);
  args->parent = thread_current()->pcb;
  args->load_success = false;
  sema_init(&args->pexec_sema, 0);
}
#undef MAX_ARGS

static void init_fork_process_args(fork_process_args* args, const struct intr_frame* if_) {
  args->if_ = if_;
  args->parent = thread_current()->pcb;
  args->load_success = false;
  sema_init(&args->pexec_sema, 0);
}

/**
 * @brief Initializes arguments for pthread creation and execution.
 * 
 * Prepares a start_pthread_args structure to pass critical context 
 * between the thread creation stub and the target pthread function.
 * 
 * @param args    Structure to initialize
 * @param sf      Stub function that wraps pthread execution
 * @param tf      Target pthread function to invoke
 * @param arg     User argument to pass to the pthread function
 */
static void init_start_pthread_args(start_pthread_args* args, stub_fun sf, pthread_fun tf,
                                    const void* arg) {
  args->sf = sf;
  args->tf = tf;
  args->arg = arg;
  args->pcb = thread_current()->pcb;
  args->success = false;
  sema_init(&args->pexec_sema, 0);
}

static void init_threads_list(Thread_list* threads) {
  lock_init(&threads->lock);
  list_init(&threads->threads);
}

/**
 * @brief Initializes a Stack_manager and preallocates thread stacks.
 * 
 * Sets up the Stack_manager structure and preallocates a fixed number 
 * of thread stacks (32 by default) in contiguous memory starting from 
 * the highest available physical address (PHYS_BASE). Stacks are 
 * allocated downward from PHYS_BASE, each PGSIZE bytes in size.
 * 
 * @param sm Pointer to the Stack_manager to initialize
 * 
 * @details
 * This function:
 * 1. Initializes the manager's lock and free stack list
 * 2. Preallocates 32 Stack_slot structures
 * 3. Assigns contiguous stack regions starting from PHYS_BASE
 * 4. Initializes the free stack list with these preallocated slots
 */
static void init_stack_manager(Stack_manager* sm) {
  lock_init(&sm->lock);
  list_init(&sm->free_stacks);
  sm->stack_base = NULL;
  sm->alloc_pgcnt = 0;

#define N_THREADS 32
  for (int i = 0; i < N_THREADS; ++i) {
    Stack_slot* ss = malloc(sizeof(Stack_slot));
    if (ss == NULL) {
      PANIC("Failed to allocate stack slot");
    }
    ss->stackpg_top = PHYS_BASE - i * PGSIZE;
    ss->stackpg_bottom = PHYS_BASE - (i + 1) * PGSIZE;
    ss->is_mapped = false;
    list_push_back(&sm->free_stacks, &ss->elem);
    ++sm->alloc_pgcnt;
  }
  sm->stack_base = PHYS_BASE - N_THREADS * PGSIZE;
#undef N_THREADS
}

/**
 * @brief Expands the stack pool by allocating additional stack slots.
 * 
 * Dynamically increases the number of available stack slots in the 
 * Stack_manager by the specified increment. New stacks are allocated 
 * contiguously below the current stack base address, growing downward.
 * 
 * @param sm        Pointer to the Stack_manager to expand
 * @param increment Number of new stack slots to allocate
 * @return true  if expansion succeeded
 * @return false if maximum stack limit (MAX_STACK_PAGES) would be exceeded
 */
static bool expand_stack_pool(Stack_manager* sm, int increment) {
  ASSERT(sm != NULL);
  ASSERT(increment > 0);
  ASSERT(lock_held_by_current_thread(&sm->lock));

  if (sm->alloc_pgcnt + increment > MAX_STACK_PAGES) {
    return false;
  }
  // Allocate new stack slots.
  void* stack_base = sm->stack_base;
  for (int i = 0; i < increment; ++i) {
    Stack_slot* ss = malloc(sizeof(Stack_slot));
    if (ss == NULL) {
      // Free previously allocated slots in case of failure.
      for (int j = 0; j < i; ++j) {
        free(list_pop_back(&sm->free_stacks));
        --sm->alloc_pgcnt;
      }
      return false;
    }
    ss->stackpg_top = stack_base - i * PGSIZE;
    ss->stackpg_bottom = stack_base - (i + 1) * PGSIZE;
    ss->is_mapped = false;
    list_push_back(&sm->free_stacks, &ss->elem);
    ++sm->alloc_pgcnt;
  }
  sm->stack_base -= increment * PGSIZE;
  return true;
}

/**
 * @brief Allocates a new stack slot from the Stack_manager.
 * 
 * Retrieves a free stack slot from the manager's pool. If the pool is 
 * empty, attempts to expand it by allocating 16 additional stack slots.
 * 
 * @param sm Pointer to the Stack_manager
 * @return Stack_slot* Pointer to the allocated stack slot on success,
 *                    NULL if maximum stack limit is reached
 */
static Stack_slot* process_allocate_stack(Stack_manager* sm) {
  ASSERT(sm != NULL);

  lock_acquire(&sm->lock);
  if (list_empty(&sm->free_stacks)) {
    // Expand the stack pool if we run out of free stacks.
    if (!expand_stack_pool(sm, 16)) {
      lock_release(&sm->lock);
      return NULL;
    }
  }
  Stack_slot* slot = list_entry(list_pop_front(&sm->free_stacks), Stack_slot, elem);
  lock_release(&sm->lock);
  return slot;
}

/**
 * @brief Initializes all descriptor tables for a new process.
 * 
 * Sets up the process's file descriptors, synchronization primitives,
 * and ELF file pointer to their initial states (NULL). This ensures the
 * process starts with a clean slate for resource management.
 * 
 * @param pcb Pointer to the process control block to initialize
 */
static void init_process_descriptors(struct process* pcb) {
  pcb->elf_file = NULL;

  // Initialize file descriptor table
  for (int i = 0; i < MAX_OPEN_FILE; ++i) {
    pcb->ofile[i] = NULL;
  }

  // Initialize synchronization primitives
  for (int i = 0; i < MAX_SYNC; ++i) {
    pcb->user_locks[i] = NULL;
    pcb->user_semaphores[i] = NULL;
  }
}

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  /* Kill the kernel if we did not succeed */
  ASSERT(success);
  t->pcb->main_thread = t;
  t->pcb->pid = get_pid(t->pcb);
  t->pcb->ppid = -1;
  t->pcb->elf_file = NULL;
  list_init(&t->pcb->children);
  init_threads_list(&t->pcb->threads);
  init_stack_manager(&t->pcb->stack_manager);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* command) {
  char* fn_copy;
  tid_t tid;

  sema_init(&temporary, 0);
  /* Make a copy of COMMAND.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, command, PGSIZE);

  start_process_args* sargs = (start_process_args*)malloc(sizeof(start_process_args));
  if (sargs == NULL)
    return TID_ERROR;
  // Initialize the args struct, before we create a new thread.
  init_start_process_args(sargs, fn_copy);

  // Since we already have parsed the command line, we can pass the name here.
  tid = thread_create(sargs->user_argv[0], PRI_DEFAULT, start_process, sargs);
  if (tid == TID_ERROR) {
    palloc_free_page(fn_copy);
    free(sargs);
    return tid;
  }

  //* start_process() will call sema_up() when it is done loading.
  //* start_process() will also clean up `fn_copy`.
  // Wait for the child to finish loading.
  sema_down(&sargs->pexec_sema);
  bool success = sargs->load_success;
  free(sargs);
  return success ? tid : TID_ERROR;
}

pid_t process_fork(const struct intr_frame* if_) {
  struct thread* cur = thread_current();
  fork_process_args* fargs = (fork_process_args*)malloc(sizeof(fork_process_args));
  if (fargs == NULL) {
    return TID_ERROR;
  }
  init_fork_process_args(fargs, if_);
  tid_t tid = thread_create(cur->pcb->process_name, PRI_DEFAULT, perform_fork_operations, fargs);

  if (tid == TID_ERROR) {
    free(fargs);
    return tid;
  }

  sema_down(&fargs->pexec_sema);
  bool success = fargs->load_success;
  free(fargs);
  return success ? tid : TID_ERROR;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* sargs) {
  start_process_args* args = (start_process_args*)sargs;
  struct process* parent = args->parent;
  char* user_cmd = (char*)args->command;
  const char** user_argv = args->user_argv;
  int argc = args->argc;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);
    t->pcb->pid = get_pid(t->pcb);
    t->pcb->ppid = parent->pid;
    list_init(&t->pcb->children);
    init_threads_list(&t->pcb->threads);
    init_stack_manager(&t->pcb->stack_manager);
    init_process_descriptors(t->pcb);
  }

  /* Make a new wait_status and insert it to parent's children list. */
  if (success) {
    Wait_status* ws = new_and_init_wait_status(t->pcb->pid);
    if (ws != NULL) {
      t->pcb->wait_status = ws;
      list_push_back(&parent->children, &ws->elem);
    }
    success = (ws != NULL);
  }

  if (success) {
    Join_status* js = new_and_init_join_status(t->tid);
    if (js != NULL) {
      t->join_status = js;
      lock_acquire(&new_pcb->threads.lock);
      list_push_back(&new_pcb->threads.threads, &js->elem);
      lock_release(&new_pcb->threads.lock);
    }
    success = (js != NULL);
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(user_argv, argc, &if_.eip, &if_.esp);
  }

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  /* Clean up. Exit on failure or jump to userspace */
  palloc_free_page(user_cmd);

  // Wake up the parent thread.
  args->load_success = success;
  sema_up(&args->pexec_sema);

  if (!success) {
    sema_up(&temporary);
    thread_exit();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {
  struct list* childrenLst = &thread_current()->pcb->children;
  struct list_elem* e;
  for (e = list_begin(childrenLst); e != list_end(childrenLst); e = list_next(e)) {
    Wait_status* ws = list_entry(e, Wait_status, elem);
    if (ws->pid == child_pid) {
      // Found the child process and wait for it.
      sema_down(&ws->dead);
      int exit_code = ws->exit_code;
      int ref_cnt = -1;
      lock_acquire(&ws->lock);
      ref_cnt = --(ws->ref_cnt);
      lock_release(&ws->lock);
      ASSERT(ref_cnt == 0);
      // Remove the child from the list and free the resources.
      list_remove(e);
      free(ws);
      return exit_code;
    }
  }

  // Child not found, simply return -1.
  return -1;
}

/* Free the current process's resources. */
void process_exit(void) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  handle_exit_close_files(cur);
  handle_exit_wait_status(cur, cur->pcb->exit_code);

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);

  sema_up(&temporary);
  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/**
 * @brief Parses the user command into an array of arguments.
 * 
 * @param user_cmd  
 * @param user_argv 
 * @param max_args 
 * @return int 
 */
static int parse_user_command(char* user_cmd, const char* user_argv[], const int max_args) {
  int argc = 0;

  char* token = NULL;
  char* save_ptr = NULL;
  for (token = strtok_r(user_cmd, " ", &save_ptr); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr)) {
    ASSERT(argc < max_args);
    user_argv[argc++] = token;
  }
  return argc;
}

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char** user_argv, int argc, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  ASSERT(argc > 0);
  ASSERT(user_argv[0] != NULL);

  const char* file_name = user_argv[0];

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Set up the arguments on the stack
    value layout on the stack:
      argv string
      argv pointer
      argc
  */

  // 1. Copy the arguments to the stack
  char* user_sp = (char*)(*esp);
  for (int i = 0; i < argc; ++i) {
    size_t arg_size = strlen(user_argv[i]) + 1;
    user_sp -= arg_size;
    strlcpy(user_sp, user_argv[i], arg_size);
    user_argv[i] = user_sp; /* Reuse */
  }

  // 2. Align the stack pointer
  uintptr_t* arg_ptr = (uintptr_t*)((uintptr_t)(user_sp)&ALIGN_MASK(uintptr_t));
  arg_ptr -= (argc + 1); /* +1 for the NULL terminator */
  for (int i = 0; i < argc; ++i) {
    arg_ptr[i] = (uintptr_t)user_argv[i];
  }
  arg_ptr[argc] = (uintptr_t)NULL;

  // 3. Set the argc and argv pointers.
  char* const argv = (char*)arg_ptr;
  arg_ptr -= 2;
  /* Aligned to a 16-byte boundary at the time the call instruction is executed */
  arg_ptr = (uintptr_t*)((uintptr_t)arg_ptr & ~0xf);
  arg_ptr[0] = (uintptr_t)argc;
  arg_ptr[1] = (uintptr_t)argv;

  // 4. Make the fake return address
  arg_ptr -= 1;
  *arg_ptr = (uintptr_t)NULL;
  *esp = arg_ptr;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  if (!success) {
    file_close(file);
  } else {
    // ! Must ensure that nobody can modify its executable on disk, so
    // ! We don't close this executable file on success.
    file_deny_write(file);
    t->pcb->elf_file = file;
  }

  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {

  Stack_slot* ss = process_allocate_stack(&thread_current()->pcb->stack_manager);
  if (ss == NULL) {
    return false;
  }

  if (ss->is_mapped) {
    // Stack already mapped, no need to allocate a new one.
    *esp = ss->stackpg_top;
    list_push_back(&thread_current()->user_stack, &ss->elem);
    return true;
  }

  uint8_t* kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  bool success = install_page(ss->stackpg_bottom, kpage, true);
  if (!success) {
    palloc_free_page(kpage);
    return false;
  }

  *esp = ss->stackpg_top;
  ss->is_mapped = true;
  list_push_back(&thread_current()->user_stack, &ss->elem);
  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void** esp, Join_status* js) {
  struct thread* t = thread_current();
  ASSERT(t->pcb != NULL);
  ASSERT(js != NULL);

  Stack_slot* ss = process_allocate_stack(&t->pcb->stack_manager);
  if (ss == NULL) {
    return false;
  }

  Thread_list* threads = &t->pcb->threads;
  lock_acquire(&threads->lock);
  list_push_back(&threads->threads, &js->elem);
  lock_release(&threads->lock);

  if (ss->is_mapped) {
    // Stack already mapped, no need to allocate a new one.
    *esp = ss->stackpg_top;
    list_push_back(&t->user_stack, &ss->elem);
    return true;
  }

  uint8_t* kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage == NULL) {
    return false;
  }

  bool success = install_page(ss->stackpg_bottom, kpage, true);
  if (!success) {
    palloc_free_page(kpage);
    return false;
  }

  *esp = ss->stackpg_top;
  ss->is_mapped = true;
  list_push_back(&t->user_stack, &ss->elem);
  return true;
}

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, const void* arg UNUSED) {
  start_pthread_args* sargs = (start_pthread_args*)malloc(sizeof(start_pthread_args));
  if (sargs == NULL)
    return TID_ERROR;

  init_start_pthread_args(sargs, sf, tf, arg);
  tid_t tid = thread_create("pthread", PRI_DEFAULT, start_pthread, sargs);
  if (tid == TID_ERROR) {
    free(sargs);
    return tid;
  }

  sema_down(&sargs->pexec_sema);
  bool success = sargs->success;
  free(sargs);

  return success ? tid : TID_ERROR;
}

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {
  start_pthread_args* args = (start_pthread_args*)exec_;
  struct thread* t = thread_current();
  bool success = false;

  ASSERT(args->pcb != NULL);
  t->pcb = args->pcb;

  // Set cr3 to the current process's page directory.
  process_activate();

  struct intr_frame if_;
  memset(&if_, 0, sizeof if_);

  /* Initialize interrupt frame. */
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  // Set the thread's function to be executed.
  if_.eip = (void (*)(void))args->sf;

  Join_status* js = new_and_init_join_status(t->tid);
  success = (js != NULL);

  if (success) {
    t->join_status = js;
    success = setup_thread(&if_.esp, js);
  }

  args->success = success;

  if (!success) {
    sema_up(&args->pexec_sema);
    thread_exit();
    NOT_REACHED();
  }

  // Set up the arguments on the stack
  if_.esp -= 16;
  uintptr_t* arg_ptr = (uintptr_t*)((uintptr_t)(if_.esp) & ALIGN_MASK(uintptr_t));

  /* Aligned to a 16-byte boundary at the time the call instruction is executed */
  arg_ptr = (uintptr_t*)((uintptr_t)arg_ptr & ~0xf);
  arg_ptr[0] = (uintptr_t)args->tf;
  arg_ptr[1] = (uintptr_t)args->arg;

  // Make the fake return address
  arg_ptr -= 1;
  *arg_ptr = (uintptr_t)NULL;
  if_.esp = arg_ptr;

  sema_up(&args->pexec_sema);
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid) {
  struct thread* cur = thread_current();
  if (tid == cur->tid || tid == TID_ERROR) {
    return TID_ERROR;
  }

  struct process* pcb = cur->pcb;
  ASSERT(pcb != NULL);

  Thread_list* thread_list = &pcb->threads;
  lock_acquire(&thread_list->lock);

  for (struct list_elem* e = list_begin(&thread_list->threads);
       e != list_end(&thread_list->threads); e = list_next(e)) {
    Join_status* js = list_entry(e, Join_status, elem);
    if (js->tid == tid) {
      lock_release(&thread_list->lock);
      sema_down(&js->dead);
      // Remove the thread from the list and free the resources.
      list_remove(e);
      free(js);
      return tid;
    }
  }

  lock_release(&thread_list->lock);
  return TID_ERROR;
}

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {
  struct thread* cur = thread_current();
  struct process* pcb = cur->pcb;
  ASSERT(pcb != NULL);

  // Release the thread's stack.
  ASSERT(list_size(&cur->user_stack) > 0);
  for (struct list_elem* e = list_begin(&cur->user_stack); e != list_end(&cur->user_stack);) {
    Stack_slot* ss = list_entry(e, Stack_slot, elem);
    e = list_remove(e);
    lock_acquire(&pcb->stack_manager.lock);
    list_push_front(&pcb->stack_manager.free_stacks, &ss->elem);
    lock_release(&pcb->stack_manager.lock);
  }

  // Wake up any waiters on this thread after we release the stack.
  sema_up(&cur->join_status->dead);

  // We let waiter to do clean up of Join_status.
  cur->join_status = NULL;
  thread_exit();
}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}

static void handle_exit_wait_status(struct thread* cur, int exit_code) {
  // Iterate through the list of children and free resources if needed.
  struct list* childrenLst = &cur->pcb->children;
  struct list_elem* e;
  int ref_cnt = -1;
  for (e = list_begin(childrenLst); e != list_end(childrenLst);) {
    Wait_status* ws = list_entry(e, Wait_status, elem);
    lock_acquire(&ws->lock);
    ref_cnt = --(ws->ref_cnt);
    lock_release(&ws->lock);
    if (ref_cnt == 0) {
      e = list_remove(e);
      free(ws);
    } else {
      e = list_next(e);
    }
  }

  Wait_status* ws = cur->pcb->wait_status;
  if (ws == NULL) {
    return;
  }

  // Tell the parent process that this child has exited.
  ref_cnt = -1;
  ws->exit_code = exit_code;
  lock_acquire(&ws->lock);
  ref_cnt = --(ws->ref_cnt);
  lock_release(&ws->lock);
  if (ref_cnt == 0) {
    // Parent has already exited, so it's now our
    // duty to free the wait status.
    free(ws);
    cur->pcb->wait_status = NULL;
  } else {
    // Parent is still alive, so we can just signal it.
    sema_up(&ws->dead);
  }
}

static Wait_status* new_and_init_wait_status(pid_t pid) {
  Wait_status* ws = (Wait_status*)malloc(sizeof(Wait_status));
  if (ws == NULL)
    return NULL;

  ws->pid = pid;
  lock_init(&ws->lock);
  ws->ref_cnt = 2;
  ws->exit_code = 0;
  sema_init(&ws->dead, 0);
  return ws;
}

/**
 * @brief Allocates and initializes a new Join_status structure.
 * 
 * Creates a Join_status object to track the termination status of a thread
 * with the specified TID. The returned structure is initialized with:
 * - The provided thread ID (tid)
 * - A semaphore ('dead') initialized to 0, used to block joining threads
 * 
 * @param tid The thread ID (tid_t) of the target thread to track
 * @return Join_status* Pointer to the initialized structure on success,
 *         NULL if memory allocation fails
 */
static Join_status* new_and_init_join_status(tid_t tid) {
  Join_status* js = (Join_status*)malloc(sizeof(Join_status));
  if (js == NULL)
    return NULL;

  js->tid = tid;
  sema_init(&js->dead, 0);
  return js;
}

static void handle_exit_close_files(struct thread* cur) {
  // Close all open files.
  for (int i = 2; i < MAX_OPEN_FILE; ++i) {
    struct file* of = cur->pcb->ofile[i];
    if (of != NULL) {
      file_close(of);
      cur->pcb->ofile[i] = NULL;
    }
  }
  // Close elf file.
  if (cur->pcb->elf_file != NULL) {
    file_close(cur->pcb->elf_file);
    cur->pcb->elf_file = NULL;
  }
}

/**
 * @brief Handles the fork operation by creating a child process context. 
 *        This function initializes a new process control block (PCB), copies 
 *        resource metadata (e.g., open files, page directory), sets up inter-process 
 *        relationships (parent/child PID tracking), and prepares the interrupt frame 
 *        for the child process to resume execution with a fresh address space. 
 *        Memory allocation failures or resource copy errors will abort the fork and 
 *        clean up allocated resources.
 * 
 * @param args Pointer to fork process arguments containing parent process reference 
 *             and interrupt frame data.
 */
static void perform_fork_operations(void* args) {
  fork_process_args* fargs = (fork_process_args*)args;
  struct process* parent = fargs->parent;
  struct thread* t = thread_current();
  struct intr_frame cur_if;

  bool success, pcb_success;

  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  if (success) {
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);
    t->pcb->pid = get_pid(t->pcb);
    t->pcb->ppid = parent->pid;
    list_init(&t->pcb->children);
    init_threads_list(&t->pcb->threads);
    init_stack_manager(&t->pcb->stack_manager);
    // Copy the user open files.
    for (int i = 0; i < MAX_OPEN_FILE; ++i) {
      t->pcb->ofile[i] = share_file(parent->ofile[i]);
    }

    t->pcb->elf_file = share_file(parent->elf_file);
    file_deny_write(t->pcb->elf_file);
    // TODO: Copy lock and semaphore objects if needed.
  }

  // Copy the page directory.
  if (success) {
    do {
      t->pcb->pagedir = pagedir_create();
      if (t->pcb->pagedir == NULL) {
        success = false;
        break;
      }
      process_activate();
      success = copy_pgd_on_fork(t->pcb->pagedir, parent->pagedir);
    } while (0);
  }

  if (success) {
    Wait_status* ws = new_and_init_wait_status(t->pcb->pid);
    if (ws != NULL) {
      t->pcb->wait_status = ws;
      list_push_back(&parent->children, &ws->elem);
    }
    success = (ws != NULL);
  }

  if (success) {
    Join_status* js = new_and_init_join_status(t->tid);
    if (js != NULL) {
      t->join_status = js;
      lock_acquire(&new_pcb->threads.lock);
      list_push_back(&new_pcb->threads.threads, &js->elem);
      lock_release(&new_pcb->threads.lock);
    }
    success = (js != NULL);
  }

  if (success) {
    memset(&cur_if, 0, sizeof cur_if);
    // Copy the parent interrupt frame to the child.
    memcpy(&cur_if, fargs->if_, sizeof(struct intr_frame));
    // Fork will return 0.
    cur_if.eax = 0;
  }

  if (!success && pcb_success) {
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  fargs->load_success = success;
  sema_up(&fargs->pexec_sema);

  if (!success) {
    thread_exit();
  }
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&cur_if) : "memory");
  NOT_REACHED();
}
