#include <random.h>
#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

#define FILE_SIZE (128 * 1024) // 128 KiB
#define BLOCKS (FILE_SIZE / 512)

static inline bool write_count_in_limits(unsigned write_cnt) { return write_cnt <= BLOCKS * 2; }

void test_main(void) {

  const char* file_name = "writemerge";
  int8_t ch = 'a';
  int fd;

  CHECK(create(file_name, FILE_SIZE), "create \"%s\"", file_name);
  CHECK((fd = open(file_name)) > 1, "open \"%s\"", file_name);

  disk_reset_cnt();

  msg("write \"%s\" with '%c' for %d bytes", file_name, ch, FILE_SIZE);

  for (int i = 0; i < FILE_SIZE; ++i) {
    // write byte by byte.
    if (write(fd, &ch, 1) <= 0) {
      fail("write \"%s\" at %d: expected to write '%c', but failed", file_name, i, ch);
    }
  }
  msg("seek \"%s\" to 0", file_name);
  seek(fd, 0);
  msg("read \"%s\" with '%c' for %d bytes", file_name, ch, FILE_SIZE);

  for (int i = 0; i < FILE_SIZE; ++i) {
    // read byte by byte.
    int8_t ch2;
    if (read(fd, &ch2, 1) <= 0) {
      fail("read \"%s\" at %d: expected to read '%c', but failed", file_name, i, ch);
    }
    if (ch2 != ch) {
      fail("read \"%s\" at %d: expected '%c', got '%d'", file_name, i, ch, ch2);
    }
  }

  msg("close \"%s\"", file_name);
  close(fd);

  unsigned write_cnt = get_disk_write_cnt();
  CHECK(write_count_in_limits(write_cnt), "write count is within limits");
}
