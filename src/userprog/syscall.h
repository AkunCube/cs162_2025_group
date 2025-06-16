#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdint.h>
void syscall_init(void);

uint32_t sys_write(uint32_t*);
uint32_t sys_practice(uint32_t*);
uint32_t sys_halt(uint32_t*);
uint32_t sys_exec(uint32_t*);
uint32_t sys_exit(uint32_t*);
uint32_t sys_wait(uint32_t*);
uint32_t sys_create(uint32_t*);
uint32_t sys_open(uint32_t*);
uint32_t sys_filesize(uint32_t*);
uint32_t sys_read(uint32_t*);
uint32_t sys_close(uint32_t*);
uint32_t sys_tell(uint32_t*);
uint32_t sys_seek(uint32_t*);
uint32_t sys_remove(uint32_t*);
uint32_t sys_compute_e(uint32_t*);
uint32_t sys_inumber(uint32_t*);
uint32_t sys_mkdir(uint32_t*);
uint32_t sys_isdir(uint32_t*);
uint32_t sys_chdir(uint32_t*);

#endif /* userprog/syscall.h */
