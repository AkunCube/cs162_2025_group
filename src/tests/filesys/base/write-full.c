#include <random.h>
#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include <stdbool.h>
#include "tests/lib.h"
#include "tests/main.h"

#define BUF_SIZE 512           // Size of the buffer to write
#define FILE_SIZE (128 * 1024) // 128 KiB

static inline bool is_far_less(unsigned a, unsigned b) {
  // Check if a is far less than b, i.e., a is less than b / 6
  return a < (b / 6);
}

void test_main(void) {
  const char* file_name = "writefull";
  static int8_t data[BUF_SIZE];
  int fd;

  CHECK(create(file_name, FILE_SIZE), "create \"%s\"", file_name);
  CHECK((fd = open(file_name)) > 1, "open \"%s\"", file_name);

  disk_reset_cnt();

  msg("write \"%s\" for %d bytes", file_name, FILE_SIZE);

  for (int i = 0; i < FILE_SIZE; i += BUF_SIZE) {
    // write byte by byte.
    if (write(fd, data, BUF_SIZE) <= 0) {
      fail("write \"%s\" at %d, but failed", file_name, i);
    }
  }
  msg("close \"%s\"", file_name);
  close(fd);

  unsigned read_cnt = get_disk_read_cnt();
  unsigned write_cnt = get_disk_write_cnt();
  CHECK(is_far_less(read_cnt, write_cnt), "read count is far less than write count");
}
