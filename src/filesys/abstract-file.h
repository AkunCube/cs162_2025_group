#ifndef FILESYS_ABSTRACT_FILE_H
#define FILESYS_ABSTRACT_FILE_H

#include <stdbool.h>

enum file_type {
  FILE_TYPE_FILE, // Normal file
  FILE_TYPE_DIR,  // Directory
};

struct abstract_file {
  enum file_type type; // Type of the file (file or directory)
};

static inline bool abstract_file_is_file(const struct abstract_file* af) {
  return af->type == FILE_TYPE_FILE;
}

static inline bool abstract_file_is_dir(const struct abstract_file* af) {
  return af->type == FILE_TYPE_DIR;
}

#define to_file(af) ((struct file*)(af))
#define to_dir(af) ((struct dir*)(af))
#define to_af(cf) ((struct abstract_file*)(cf))

#endif // FILESYS_ABSTRACT_FILE_H
