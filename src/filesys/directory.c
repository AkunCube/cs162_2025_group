#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "filesys/abstract-file.h"
#include "userprog/process.h"

/* A directory. */
struct dir {
  struct abstract_file af; /* Abstract file interface. */
  struct inode* inode;     /* Backing store. */
  off_t pos;               /* Current position. */
};

/* A single directory entry. */
struct dir_entry {
  block_sector_t inode_sector; /* Sector number of header. */
  char name[NAME_MAX + 1];     /* Null terminated file name. */
  enum file_type type;         /* Type of the file (file or directory). */
  bool in_use;                 /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool dir_create(block_sector_t sector, size_t entry_cnt) {
  return inode_create(sector, entry_cnt * sizeof(struct dir_entry));
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir* dir_open(struct inode* inode) {
  struct dir* dir = calloc(1, sizeof *dir);
  if (inode != NULL && dir != NULL) {
    dir->af.type = FILE_TYPE_DIR;
    dir->inode = inode;
    dir->pos = 0;
    return dir;
  } else {
    inode_close(inode);
    free(dir);
    return NULL;
  }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir* dir_open_root(void) {
  return dir_open(inode_open(ROOT_DIR_SECTOR, FILE_TYPE_DIR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir* dir_reopen(struct dir* dir) {
  return dir_open(inode_reopen(dir->inode));
}

/* Destroys DIR and frees associated resources. */
void dir_close(struct dir* dir) {
  if (dir != NULL) {
    inode_close(dir->inode);
    free(dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode* dir_get_inode(struct dir* dir) {
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool lookup(const struct dir* dir, const char* name, struct dir_entry* ep, off_t* ofsp) {
  struct dir_entry e;
  size_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (e.in_use && !strcmp(name, e.name)) {
      if (ep != NULL)
        *ep = e;
      if (ofsp != NULL)
        *ofsp = ofs;
      return true;
    }
  return false;
}

/**
 * @brief Looks up a directory entry by name and returns its inode.
 * @param dir The directory to search in.
 * @param name The name of the entry to look up.
 * @param inode Output parameter: pointer to the found inode, or NULL if not found.
 * @param ofsp Output parameter: offset of the directory entry, or undefined on failure.
 * @return true if the entry was found and the inode was successfully opened;
 *         false otherwise (entry not found or inode open failed).
 */
bool dir_lookup(const struct dir* dir, const char* name, struct inode** inode, off_t* ofsp) {
  struct dir_entry e;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  if (lookup(dir, name, &e, ofsp))
    *inode = inode_open(e.inode_sector, e.type);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool dir_add(struct dir* dir, const char* name, block_sector_t inode_sector, enum file_type type) {
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen(name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup(dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy(e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  e.type = type;
  success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/**
 * @brief Removes a directory entry by marking it as unused and freeing its inode.
 * @param dir The directory containing the entry to remove.
 * @param inode The inode of the entry to remove.
 * @param ofs The offset of the directory entry within the directory.
 * @return true if the entry was successfully removed;
 *         false if the entry could not be removed (e.g., root directory, non-empty directory, inode in use).
 * @details This function marks the directory entry as unused and decrements the directory's entry count.
 *          The root directory, non-empty directories, and directories in use (open or current working directory)
 *          cannot be removed. The inode is always closed, even on failure.
 */
bool dir_remove(struct dir* dir, struct inode* inode, off_t ofs) {
  ASSERT(dir != NULL);
  ASSERT(inode != NULL);
  bool success = false;

  if (inode == NULL || inode_get_inumber(inode) == ROOT_DIR_SECTOR) {
    // Cannot remove the root directory.
    goto cleanup;
  }

  if (inode_isdir(inode) && (!inode_directory_is_empty(inode) || inode_get_open_count(inode) > 1 ||
                             process_cwd_matches_sector(inode_get_inumber(inode)))) {
    // Cannot remove a non-empty directory or one that is currently in use.
    goto cleanup;
  }

  struct dir_entry e = {.in_use = false};
  success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;
  if (success) {
    inode_remove(inode);
    decrement_dir_entry_count(dir->inode, 1);
  }

cleanup:
  inode_close(inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool dir_readdir(struct dir* dir, char name[NAME_MAX + 1]) {
  struct dir_entry e;

  while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
    dir->pos += sizeof e;
    if (!e.in_use)
      continue; // Skip free entries.
    if (!strcmp(e.name, ".") || !strcmp(e.name, ".."))
      continue; // Skip special entries.

    strlcpy(name, e.name, NAME_MAX + 1);
    return true;
  }
  return false;
}

size_t dir_entry_size(void) { return sizeof(struct dir_entry); }
