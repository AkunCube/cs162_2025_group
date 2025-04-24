#include "userprog/syscall.h"
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

static uint32_t (*syscalls[])(uint32_t*) = {
    [SYS_WRITE] = sys_write,
};

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */

  if (args[0] == SYS_EXIT) {
    f->eax = args[1];
    printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
    process_exit();
  } else {
    f->eax = syscalls[args[0]](args);
  }
}

uint32_t sys_write(uint32_t* args) {
  int fd = (int)args[1];
  const char* buffer = (const char*)args[2];
  unsigned size = (unsigned)args[3];

  if (fd == STDOUT_FILENO) {
    putbuf(buffer, size);
    return size;
  } else {
    ASSERT(false);
  }

  return 0;
}

uint32_t sys_exit(uint32_t* args) { return 0; }
