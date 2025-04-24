#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdint.h>
void syscall_init(void);

uint32_t sys_write(uint32_t*);

#endif /* userprog/syscall.h */
