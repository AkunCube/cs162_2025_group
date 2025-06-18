#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "filesys/abstract-file.h"
#include <string.h>

/* Partition that contains the file system. */
struct block* fs_device;

static void do_format(void);
static inline bool path_is_absolute(const char* path);
static struct dir* setup_directory_context(const char* path, char** path_copy);
static bool create_final_component(char* path, struct dir* current_dir, off_t initial_size,
                                   enum file_type type);
static struct inode* resolve_last_path_component(struct dir* current_dir, char* path);
static void initialize_dir_special_entries(struct dir* dir, block_sector_t current_inode_sector,
                                           block_sector_t parent_inode_sector);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  inode_init();
  init_file_caches();
  free_map_init();

  if (format)
    do_format();

  free_map_open();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void) { free_map_close(); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create(const char* path, off_t initial_size) {
  char* path_copy = NULL;
  struct dir* current_dir = setup_directory_context(path, &path_copy);
  if (!current_dir) {
    return false;
  }
  ASSERT(path_copy != NULL);

  bool success = create_final_component(path_copy, current_dir, initial_size, FILE_TYPE_FILE);
  free(path_copy);
  return success;
}

/**
 * @brief Opens a file or directory by path and returns an abstract file handle.
 * Resolves the path, checks file type (regular file or directory), 
 * and initializes the appropriate abstract file structure.
 * @param name Path of the file or directory to open.
 * @return struct abstract_file* Pointer to the initialized abstract file on success, 
 *                              NULL on failure (e.g., path not found, permission denied).
 */
struct abstract_file* filesys_open(const char* name) {
  char* path_copy = NULL;
  struct dir* current_dir = setup_directory_context(name, &path_copy);
  if (!current_dir) {
    return NULL;
  }
  ASSERT(path_copy != NULL);
  struct inode* inode = resolve_last_path_component(current_dir, path_copy);

  free(path_copy);
  if (inode == NULL) {
    return NULL;
  }

  if (inode_isdir(inode)) {
    return to_af(dir_open(inode));
  } else {
    return to_af(file_open(inode));
  }
}

/**
 * @brief Changes the current working directory to the specified path.
 * 
 * Resolves the given path to a directory and updates the current working 
 * directory of the calling process. The path can be absolute or relative.
 * 
 * @param path The path of the directory to set as CWD.
 * @return true if the directory was found and CWD was updated, false otherwise.
 */
bool filesys_chdir(const char* path) {
  char* path_copy = NULL;
  struct dir* current_dir = setup_directory_context(path, &path_copy);
  if (!current_dir) {
    return false;
  }
  ASSERT(path_copy != NULL);

  struct inode* inode = resolve_last_path_component(current_dir, path_copy);
  bool success = inode != NULL && inode_isdir(inode);

  if (success) {
    process_set_cwd(inode_get_inumber(inode));
  }

  free(path_copy);
  inode_close(inode);
  return success;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char* name) {
  struct dir* dir = dir_open_root();
  bool success = dir != NULL && dir_remove(dir, name);
  dir_close(dir);

  return success;
}

/**
 * @brief Creates a new directory at the specified path.
 *
 * This function creates a single directory. The parent directory must exist.
 * For example, to create "/a/b/c", directories "/a" and "/a/b" must already exist.
 *
 * @param path The path of the directory to create. Can be absolute or relative.
 * @return true if the directory was created successfully, false otherwise.
 */
bool filesys_mkdir(const char* path) {
  char* path_copy = NULL;
  struct dir* current_dir = setup_directory_context(path, &path_copy);
  if (!current_dir) {
    return false;
  }
  ASSERT(path_copy != NULL);

  bool success = create_final_component(path_copy, current_dir, 0, FILE_TYPE_DIR);
  free(path_copy);
  return success;
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}

static inline bool path_is_absolute(const char* path) {
  ASSERT(path != NULL);
  return *path == '/';
}

/**
 * @brief Sets up the directory context for path resolution and returns the initial directory.
 * 
 * Creates a copy of the input path, processes it to determine the starting directory 
 * (root for absolute paths or current working directory for relative paths), and returns 
 * the directory context. The path copy is stored at the provided pointer for subsequent use.
 * 
 * @param path The input path to process (absolute or relative).
 * @param path_copy Pointer to store the copied and trimmed path string.
 * @return struct dir* A pointer to the initial directory context, or NULL on failure.
 */
static struct dir* setup_directory_context(const char* path, char** path_copy) {
  ASSERT(path != NULL);
  ASSERT(path_copy != NULL);

  *path_copy = NULL;
  size_t path_len = strlen(path);
  if (path_len == 0) {
    return NULL;
  }

  // Make a copy of the path for tokenization
  char* tmp = malloc(path_len + 1);
  if (tmp == NULL) {
    return NULL;
  }
  strlcpy(tmp, path, path_len + 1);
  trim(tmp); // Remove leading/trailing spaces

  struct dir* current_dir = NULL;
  if (path_is_absolute(tmp)) {
    current_dir = dir_open_root();
  } else {
    current_dir = process_cwd(thread_current()->pcb);
  }

  if (current_dir == NULL) {
    free(tmp);
    return NULL;
  }

  *path_copy = tmp;
  return current_dir;
}

/**
 * @brief Creates the final component (file or directory) in an existing path.
 * 
 * This function assumes all intermediate directories in the path already exist.
 * It will only create the final component if it does not already exist.
 * The current_dir context is automatically closed after the operation.
 * 
 * @param path The full path including the final component to create.
 * @param current_dir The directory context of the parent directory.
 * @param initial_size The initial size for the created file (ignored for directories).
 * @param type The type of the final component (FILE_TYPE_DIR or FILE_TYPE_FILE).
 * @return true if the final component was created or already exists, false on error.
 */
static bool create_final_component(char* path, struct dir* current_dir, off_t initial_size,
                                   enum file_type type) {
  char *component, *save_ptr;
  bool success = false;

  // Split the path into components and process each one
  for (component = strtok_r(path, "/", &save_ptr); component != NULL;) {
    char* last_component = component;
    component = strtok_r(NULL, "/", &save_ptr);
    struct inode* inode = NULL;

    // Check if current component exists in the directory.
    if (dir_lookup(current_dir, last_component, &inode)) {
      // Fail if component exists but is not a directory.
      if (!inode_isdir(inode)) {
        inode_close(inode);
        break;
      }

      // Move into the existing directory.
      dir_close(current_dir);
      current_dir = dir_open(inode);
      if (current_dir == NULL) {
        break;
      }
      continue;
    }

    // Fail if intermediate component is missing (only final component can be created).
    if (component != NULL) {
      break;
    }

    // Create the final component.
    block_sector_t inode_sector = 0;
    // Allocate inode and add to directory.
    if (!free_map_allocate(1, &inode_sector) || !inode_create(inode_sector, initial_size) ||
        !dir_add(current_dir, last_component, inode_sector, type)) {
      if (inode_sector != 0) {
        free_map_release(inode_sector, 1);
      }
      break;
    }

    increment_dir_entry_count(dir_get_inode(current_dir), 1);

    if (type != FILE_TYPE_DIR) {
      success = true; // Non-directory types are created directly.
      break;
    }

    // Add . and .. entries for newly created directory.
    struct dir* new_dir = dir_open(inode_open(inode_sector, FILE_TYPE_DIR));
    if (new_dir == NULL) {
      free_map_release(inode_sector, 1);
      break;
    }
    // Self and parent references.
    initialize_dir_special_entries(new_dir, inode_sector,
                                   inode_get_inumber(dir_get_inode(current_dir)));
    dir_close(new_dir);
    success = true;
  }

  dir_close(current_dir);
  return success;
}

/**
 * @brief Finds the inode of the last component in a path within a directory context.
 * 
 * Traverses the path component by component, ensuring each intermediate 
 * component is a directory, and returns the inode of the final component.
 * If any intermediate component is not a directory or does not exist, returns NULL.
 * The current_dir context is automatically closed after the operation.
 * 
 * @param current_dir The starting directory context.
 * @param path The path to resolve, relative to current_dir.
 * @return The inode of the last component, or NULL on error.
 */
static struct inode* resolve_last_path_component(struct dir* current_dir, char* path) {
  ASSERT(current_dir != NULL);
  ASSERT(path != NULL);
  char *component, *save_ptr;
  struct inode* inode = NULL;

  if (strlen(path) == 0) {
    goto cleanup;
  }

  component = strtok_r(path, "/", &save_ptr);
  if (component == NULL) {
    // Handle the edge case where the path is "/" or ends with "/".
    // Since the caller has already opened the root directory
    // (verified by path_is_absolute()), return its inode directly.
    inode = inode_reopen(dir_get_inode(current_dir));
    goto cleanup;
  }

  do {
    char* last_component = component;
    component = strtok_r(NULL, "/", &save_ptr);

    if (strlen(last_component) == 0) {
      continue;
    }

    if (!dir_lookup(current_dir, last_component, &inode)) {
      // Component not found.
      break;
    }

    // If this is the final component, we're done.
    if (component == NULL) {
      break;
    }
    // For intermediate components, must be a directory.
    if (!inode_isdir(inode)) {
      inode_close(inode);
      inode = NULL;
      break;
    }

    // Move to next directory level.
    dir_close(current_dir);
    current_dir = dir_open(inode);
    if (current_dir == NULL) {
      inode = NULL;
      break;
    }
  } while (component != NULL);

cleanup:
  dir_close(current_dir);
  return inode;
}

/**
 * @brief Initializes "." and ".." entries for a directory.
 * @param dir The directory to initialize.
 * @param current_inode_sector Inode sector for ".".
 * @param parent_inode_sector Inode sector for "..".
 */
static void initialize_dir_special_entries(struct dir* dir, block_sector_t current_inode_sector,
                                           block_sector_t parent_inode_sector) {
  dir_add(dir, ".", current_inode_sector, FILE_TYPE_DIR);
  dir_add(dir, "..", parent_inode_sector, FILE_TYPE_DIR);
}

/**
 * @brief Retrieves the inode number from an abstract file object.
 * @param af Pointer to the abstract file structure (file or directory).
 * @return The inode number if valid, -1 if the input is NULL.
 */
int filesys_get_inumber(const struct abstract_file* af) {
  if (af == NULL) {
    return -1;
  }

  struct inode* inode = NULL;
  if (abstract_file_is_file(af)) {
    inode = file_get_inode(to_file(af));
  } else if (abstract_file_is_dir(af)) {
    inode = dir_get_inode(to_dir(af));
  } else {
    PANIC("Invalid abstract file type");
  }

  return inode_get_inumber(inode);
}

/**
 * @brief Closes an abstract file handle (either a regular file or directory).
 * @param af Pointer to the abstract file structure to close. Must not be NULL.
 */
void filesys_close(struct abstract_file* af) {
  ASSERT(af != NULL);
  if (abstract_file_is_file(af)) {
    file_close(to_file(af));
  } else if (abstract_file_is_dir(af)) {
    dir_close(to_dir(af));
  } else {
    PANIC("Invalid abstract file type passed to filesys_close()");
  }
}
