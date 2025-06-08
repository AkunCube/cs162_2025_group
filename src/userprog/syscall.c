#include "userprog/syscall.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "stddef.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "debug.h"
#include <float.h>

static void syscall_handler(struct intr_frame*);
static void validate_buffer_in_user_region(const void* buffer, size_t size);
static void validate_string_in_user_region(const char* string);
static struct file* validate_file_descriptor(int fd);
static int fd_alloc(struct process* pcb);
// static struct lock fileop_lock;
void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  // lock_init(&fileop_lock);
}

static uint32_t (*syscalls[])(uint32_t*) = {
    [SYS_WRITE] = sys_write,   [SYS_PRACTICE] = sys_practice, [SYS_HALT] = sys_halt,
    [SYS_EXEC] = sys_exec,     [SYS_EXIT] = sys_exit,         [SYS_WAIT] = sys_wait,
    [SYS_CREATE] = sys_create, [SYS_OPEN] = sys_open,         [SYS_FILESIZE] = sys_filesize,
    [SYS_READ] = sys_read,     [SYS_CLOSE] = sys_close,       [SYS_TELL] = sys_tell,
    [SYS_SEEK] = sys_seek,     [SYS_REMOVE] = sys_remove,     [SYS_COMPUTE_E] = sys_compute_e,
};

static void syscall_handler(struct intr_frame* f) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */

  /* Validate the syscall number */
  validate_buffer_in_user_region(args, sizeof(uint32_t));
  int syscall_num = (int)args[0];

  int return_value = -1;

  if (syscall_num == SYS_FORK) {
    return_value = process_fork(f);
  } else {
    // Passing the real arguments to the syscall function.
    return_value = syscalls[syscall_num](&args[1]);
  }

  f->eax = return_value;
}

uint32_t sys_write(uint32_t* args) {
  validate_buffer_in_user_region(args, 3 * sizeof(uint32_t));
  int fd = (int)args[0];
  const char* buffer = (const char*)args[1];
  unsigned size = (unsigned)args[2];

  validate_buffer_in_user_region(buffer, size);
  if (fd < 0 || fd >= MAX_OPEN_FILE) {
    return -1;
  }

  if (fd == STDOUT_FILENO) {
    putbuf(buffer, size);
    return size;
  } else if (fd == STDIN_FILENO) {
    return -1;
  } else {
    struct file* of = thread_current()->pcb->ofile[fd];
    if (of == NULL) {
      return -1;
    }

    off_t bytes_written = file_write(of, buffer, size);
    return bytes_written;
  }

  return 0;
}

uint32_t sys_exit(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  int exit_code = (int)args[0];
  thread_terminate(exit_code);
  NOT_REACHED();
}

uint32_t sys_practice(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  int i = (int)args[0];
  return i + 1;
}

uint32_t sys_halt(uint32_t* args UNUSED) {
  shutdown_power_off();
  NOT_REACHED();
}

uint32_t sys_exec(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  validate_string_in_user_region((const char*)args[0]);
  const char* cmd = (const char*)args[0];
  return (uint32_t)process_execute(cmd);
}

uint32_t sys_wait(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  pid_t pid = (pid_t)args[0];
  return process_wait(pid);
}

uint32_t sys_create(uint32_t* args) {
  validate_buffer_in_user_region(args, 2 * sizeof(uint32_t));
  const char* file_name = (const char*)args[0];
  unsigned initial_size = (unsigned)args[1];
  validate_string_in_user_region((const char*)args[0]);

  bool success = filesys_create(file_name, initial_size);

  return success;
}

uint32_t sys_open(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  const char* file_name = (const char*)args[0];
  validate_string_in_user_region(file_name);

  struct file* of = NULL;
  of = filesys_open(file_name);
  if (of == NULL) {
    return -1;
  }
  struct process* pcb = thread_current()->pcb;
  int fd = fd_alloc(pcb);
  if (fd > 0) {
    pcb->ofile[fd] = of;
  }
  return fd;
}

uint32_t sys_filesize(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  int fd = (int)args[0];

  struct file* of = validate_file_descriptor(fd);
  if (of == NULL) {
    return -1;
  }

  off_t size = file_length(of);
  return size;
}

uint32_t sys_read(uint32_t* args) {
  validate_buffer_in_user_region(args, 3 * sizeof(uint32_t));
  int fd = (int)args[0];
  uint8_t* buffer = (uint8_t*)args[1];
  unsigned size = (unsigned)args[2];
  validate_buffer_in_user_region(buffer, size);
  if (fd < 0 || fd >= MAX_OPEN_FILE) {
    return -1;
  }

  if (fd == STDIN_FILENO) {
    unsigned i = 0;
    uint8_t ch;

    for (i = 0; i < size; ++i) {
      ch = buffer[i] = input_getc();
      if (ch == '\n') {
        break;
      }
    }

    return i;
  } else {
    struct file* of = thread_current()->pcb->ofile[fd];
    if (of == NULL) {
      return -1;
    }

    off_t bytes_read = file_read(of, buffer, size);

    return bytes_read;
  }
}

uint32_t sys_close(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  int fd = (int)args[0];

  struct file* of = validate_file_descriptor(fd);
  if (of == NULL) {
    return -1;
  }

  file_close(of);
  thread_current()->pcb->ofile[fd] = NULL;
  return 0;
}

uint32_t sys_tell(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  int fd = (int)args[0];

  struct file* of = validate_file_descriptor(fd);
  if (of == NULL) {
    return -1;
  }
  off_t res = file_tell(of);
  return res;
}

uint32_t sys_seek(uint32_t* args) {
  validate_buffer_in_user_region(args, 2 * sizeof(uint32_t));
  int fd = (int)args[0];
  unsigned position = (unsigned)args[1];

  struct file* of = validate_file_descriptor(fd);
  if (of == NULL) {
    return -1;
  }

  file_seek(of, position);
  return 0;
}

uint32_t sys_remove(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  const char* file_name = (const char*)args[0];
  validate_string_in_user_region(file_name);

  bool success = filesys_remove(file_name);
  return success;
}

uint32_t sys_compute_e(uint32_t* args) {
  validate_buffer_in_user_region(args, 1 * sizeof(uint32_t));
  int n = (int)args[0];
  return sys_sum_to_e(n);
}

/********************************************************/
/* HELPER FUNCTIONS */

/**
 * @brief Validate the buffer is in the user region.
 * 
 * @param buffer 
 * @param size 
 */
static void validate_buffer_in_user_region(const void* buffer, size_t size) {
  if (buffer == NULL || !is_user_vaddr(buffer)) {
    thread_terminate(-1);
  }

  // Check if the buffer is within the user address space.
  size_t delta = PHYS_BASE - buffer;
  if (size > delta) {
    thread_terminate(-1);
  }

  // Check all pages in the buffer.
  void* const buffer_end_pg = pg_round_down(buffer + size - 1);

  for (void* p = pg_round_down(buffer); p <= buffer_end_pg; p += PGSIZE) {
    if (!pagedir_get_page(thread_current()->pcb->pagedir, p)) {
      thread_terminate(-1);
    }
  }
}

/**
 * @brief Validate the string is in the user region.
 * 
 * @param string 
 */
static void validate_string_in_user_region(const char* string) {
  if (string == NULL || !is_user_vaddr(string)) {
    thread_terminate(-1);
  }

  size_t delta = PHYS_BASE - (const void*)string;
  // Becareful of the string length, it may be larger than the delta.
  // We cannot check the mapped pages, so when strnlen find a page fault, kernel will
  // go to the `page_fault()` handler. We let the `page_fault()` handler to handle the error.
  if (strnlen(string, delta) == delta) {
    thread_terminate(-1);
  }
}

/**
 * @brief Allocate a file descriptor for the process.
 * 
 * @param pcb
 * @return int
 */
static int fd_alloc(struct process* pcb) {
  for (int i = 2; i < MAX_OPEN_FILE; i++) {
    if (pcb->ofile[i] == NULL) {
      return i;
    }
  }

  return -1;
}

/**
 * @brief Validate the file descriptor.
 * 
 * @param fd 
 * @return struct file* Return the file pointer if valid, otherwise NULL.
 */
static struct file* validate_file_descriptor(int fd) {
  if (fd < 0 || fd >= MAX_OPEN_FILE) {
    return NULL;
  }

  return thread_current()->pcb->ofile[fd];
}