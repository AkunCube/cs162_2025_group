#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/abstract-file.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_POINTERS 120   /* Number of direct pointers in inode. */
#define INDIRECT_POINTERS 128 /* Number of pointers in an indirect block. */
#define MAX_SINGLE_INDIRECT_SIZE                                                                   \
  (DIRECT_POINTERS + INDIRECT_POINTERS) /* Total pointers in inode. */

#define MAX_TMP_SECTORS 130 /* Maximum number of sectors for temporary storage. */

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t direct[DIRECT_POINTERS]; /* Direct pointers to data sectors. */
  block_sector_t indirect;                /* Indirect pointer to data sectors. */
  block_sector_t doubly_indirect;         /* Doubly indirect pointer to data sectors. */
  off_t length;                           /* File size in bytes. */
  unsigned magic;                         /* Magic number. */
  uint32_t dir_entry_count;               /* Number of directory entries (for directories). */
  uint32_t unused[3];                     /* Unused space for future expansion. */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct lock lock;       /* Lock for synchronizing access. */
  struct inode_disk data; /* Inode content. */
  enum file_type type;    /* Type of the file (file or directory). */
};

static bool extend_file_sectors(struct inode_disk* disk_inode, off_t offset, off_t length);
static size_t calculate_required_sectors(size_t sectors);
static bool initialize_sectors_to_zeros(struct inode_disk* disk_inode, off_t start, off_t end);

static inline bool is_direct_sector(block_idx_t sector_idx) { return sector_idx < DIRECT_POINTERS; }

static inline bool is_indirect_sector(block_idx_t sector_idx) {
  return !is_direct_sector(sector_idx) && sector_idx < MAX_SINGLE_INDIRECT_SIZE;
}

static inline bool is_doubly_indirect_sector(block_idx_t sector_idx) {
  return sector_idx >= MAX_SINGLE_INDIRECT_SIZE;
}

static inline size_t indirect_table_index(block_idx_t sector_idx) {
  return sector_idx - DIRECT_POINTERS;
}

static inline off_t indirect_table_offset(block_idx_t sector_idx) {
  return indirect_table_index(sector_idx) * sizeof(block_sector_t);
}

static inline size_t doubly_indirect_first_level_index(block_idx_t sector_idx) {
  return (sector_idx - MAX_SINGLE_INDIRECT_SIZE) / INDIRECT_POINTERS;
}

static inline size_t doubly_indirect_second_level_index(block_idx_t sector_idx) {
  return (sector_idx - MAX_SINGLE_INDIRECT_SIZE) % INDIRECT_POINTERS;
}

/**
 * @brief Translates a byte position within a file to the corresponding disk sector.
 * 
 * @param inode   Pointer to the inode structure of the file.
 * @param pos     Byte position within the file to translate.
 * @param skip_length_check If true, skip validation of the position against the file length.
 * @return The disk sector number corresponding to the byte position, or BLOCK_NOT_FOUND if invalid.
 */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos, bool skip_length_check) {
  ASSERT(inode != NULL);

  if (pos >= inode->data.length && !skip_length_check) {
    return BLOCK_NOT_FOUND;
  }

  block_idx_t idx = pos / BLOCK_SECTOR_SIZE;

  if (is_direct_sector(idx)) {
    /* Direct pointers. */
    return inode->data.direct[idx];
  }

  if (is_indirect_sector(idx)) {
    /* Indirect pointers. */
    block_sector_t indirect_block;
    read_from_cache(inode->data.indirect, &indirect_block, indirect_table_offset(idx),
                    sizeof(block_sector_t));
    return indirect_block;
  }

  // Doubly indirect pointers.
  ASSERT(is_doubly_indirect_sector(idx));
  size_t first_level = doubly_indirect_first_level_index(idx);
  size_t second_level = doubly_indirect_second_level_index(idx);

  block_sector_t first_level_sector;
  block_sector_t data_sector;
  read_from_cache(inode->data.doubly_indirect, &first_level_sector,
                  first_level * sizeof(block_sector_t), sizeof(block_sector_t));
  read_from_cache(first_level_sector, &data_sector, second_level * sizeof(block_sector_t),
                  sizeof(block_sector_t));
  return data_sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length) {
  struct inode_disk* disk_inode = NULL;
  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);

  if (disk_inode == NULL) {
    return false; // Memory allocation failed.
  }

  // Initialize the inode structure.
  disk_inode->magic = INODE_MAGIC;
  disk_inode->length = 0;
  disk_inode->dir_entry_count = 0;

  if (!extend_file_sectors(disk_inode, 0, length)) {
    return false;
  }

  disk_inode->length = length;

  // Initialize all data sectors to zero.
  initialize_sectors_to_zeros(disk_inode, 0, length);

  // Finally, write the inode to disk.
  write_to_cache(sector, disk_inode, 0, BLOCK_SECTOR_SIZE);

  free(disk_inode);
  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector, enum file_type type) {
  struct list_elem* e;
  struct inode* inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->type = type;
  lock_init(&inode->lock);
  read_from_cache(inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL) {
    lock_acquire(&inode->lock);
    inode->open_cnt++;
    lock_release(&inode->lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire(&inode->lock);
  int open_cnt = --inode->open_cnt;
  lock_release(&inode->lock);

  if (open_cnt > 0)
    return;

  list_remove(&inode->elem);

  if (!inode->removed) {
    free(inode);
    return;
  }

  // Free the sectors allocated for this inode.

  size_t remaining = DIV_ROUND_UP(inode->data.length, BLOCK_SECTOR_SIZE);

  {
    size_t const direct_pointers = MIN(DIRECT_POINTERS, remaining);
    for (size_t i = 0; i < direct_pointers; ++i) {
      free_map_release(inode->data.direct[i], 1);
    }
    remaining -= direct_pointers;
  }

  // Free indirect pointer if it exists.
  if (remaining > 0) {
    static block_sector_t indirect_block[INDIRECT_POINTERS];
    read_from_cache(inode->data.indirect, indirect_block, 0, BLOCK_SECTOR_SIZE);

    size_t const indirect_pointers = MIN(INDIRECT_POINTERS, remaining);
    for (size_t i = 0; i < indirect_pointers; ++i) {
      free_map_release(indirect_block[i], 1);
    }
    free_map_release(inode->data.indirect, 1);
    remaining -= indirect_pointers;
  }

  // Free doubly indirect pointer if it exists.
  if (remaining > 0) {
    static block_sector_t first_level_block[INDIRECT_POINTERS];
    read_from_cache(inode->data.doubly_indirect, first_level_block, 0, BLOCK_SECTOR_SIZE);

    size_t first_level = ROUND_UP(remaining, INDIRECT_POINTERS);
    for (size_t i = 0; i < first_level; ++i) {
      static block_sector_t second_level_block[INDIRECT_POINTERS];
      read_from_cache(first_level_block[i], second_level_block, 0, BLOCK_SECTOR_SIZE);

      size_t const second_level_pointers = MIN(INDIRECT_POINTERS, remaining);
      for (size_t j = 0; j < second_level_pointers; ++j) {
        free_map_release(second_level_block[j], 1);
      }
      free_map_release(first_level_block[i], 1);
      remaining -= second_level_pointers;
    }
    free_map_release(inode->data.doubly_indirect, 1);
  }

  ASSERT(remaining == 0); // All sectors should be freed.

  free(inode);
  return;
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset, /*skip_length_check=*/false);
    // If sector_idx is -1, it means the offset is beyond the inode's length.
    if (sector_idx == BLOCK_NOT_FOUND) {
      break; // End of file reached.
    }
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      read_from_cache(sector_idx, buffer + bytes_read, 0, chunk_size);
    } else {
      /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
      read_from_cache(sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  off_t file_length = inode_length(inode); // Current length of the inode.
  bool extended = false;                   // Indicates if the inode was extended.

  // Handle the case where the offset is beyond the current length of the inode.
  if (byte_to_sector(inode, offset, /*skip_length_check=*/false) == BLOCK_NOT_FOUND) {
    if (!extend_file_sectors(&inode->data, offset, size)) {
      return 0;
    }
    initialize_sectors_to_zeros(&inode->data, inode->data.length, offset);
    extended = true;
    // We cannot set `inode->data.length` here because we haven't written the new data yet.
    // See test file `syn-rw`.
    file_length = offset + size;
  }

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    //! IMPORTANT: We haven't update the inode length yet, so we just skip the length check.
    block_sector_t sector_idx = byte_to_sector(inode, offset, /*skip_length_check=*/true);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = file_length - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    write_to_cache(sector_idx, buffer + bytes_written, sector_ofs, chunk_size);

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  // Update the inode length if we extended it.
  if (extended) {
    inode->data.length = file_length;
    // Write the updated inode data back to cache.
    write_to_cache(inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
  }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) { return inode->data.length; }

/**
 * @brief Extend the disk sectors allocated to a file's inode from a specified offset.
 * 
 * This function allocates new sectors for a file, starting from the given offset, 
 * and updates the inode's direct/indirect/doubly-indirect pointers accordingly. It 
 * handles sector allocation, index block management, and error recovery.
 * 
 * @param disk_inode Pointer to the on-disk inode structure of the file.
 * @param offset The byte offset to start the extension from.
 * @param size The number of bytes to extend the file by.
 * @return true if the extension was successful, false on failure.
 */
static bool extend_file_sectors(struct inode_disk* disk_inode, off_t offset, off_t size) {
  ASSERT(disk_inode != NULL);

  const size_t old_sectors = bytes_to_sectors(disk_inode->length);
  const size_t new_sectors = bytes_to_sectors(offset + size);
  if (old_sectors == new_sectors) {
    return true; // No extension needed
  }

  size_t sectors_to_add = new_sectors - old_sectors;

  // Calculate how many new sectors we need to allocate.
  // First calculate the number of sectors needed for the old inode.
  size_t total_needed =
      calculate_required_sectors(new_sectors) - calculate_required_sectors(old_sectors);
  block_sector_t* sector_list = malloc(total_needed * sizeof(block_sector_t));
  if (sector_list == NULL) {
    return false; // Memory allocation failed.
  }

  bool success = false; // Indicates if the allocation was successful.

  size_t allocated = 0; // Number of sectors allocated for the inode.
  for (size_t i = 0; i < total_needed; ++i, ++allocated) {
    if (!free_map_allocate(1, &sector_list[allocated])) {
      goto cleanup; // Allocation failed, clean up.
    }
  }

  block_idx_t current_sector = DIV_ROUND_UP(disk_inode->length, BLOCK_SECTOR_SIZE);
  block_idx_t const old_end_idx = disk_inode->length / BLOCK_SECTOR_SIZE;
  size_t sector_idx = 0;

  //* 1. Handle direct pointer allocation.
  while (is_direct_sector(current_sector) && (sectors_to_add != 0)) {
    disk_inode->direct[current_sector] = sector_list[sector_idx++];
    current_sector++;
    sectors_to_add--;
  }

  //* 2. Handle indirect block allocation
  if (is_indirect_sector(current_sector) && (sectors_to_add != 0)) {
    // Calculate how many sectors we can fill in the indirect block.
    size_t indirect_alloc = MIN(sectors_to_add, MAX_SINGLE_INDIRECT_SIZE - current_sector);

    block_sector_t* indirect_block = calloc(indirect_alloc, sizeof(block_sector_t));
    if (indirect_block == NULL) {
      goto cleanup;
    }

    if (is_direct_sector(old_end_idx)) {
      // We need to allocate a new indirect pointer.
      disk_inode->indirect = sector_list[sector_idx++];
    }
    for (size_t i = 0; i < indirect_alloc; ++i) {
      indirect_block[i] = sector_list[sector_idx++];
    }

    write_to_cache(disk_inode->indirect, indirect_block, indirect_table_offset(current_sector),
                   indirect_alloc * sizeof(block_sector_t));
    free(indirect_block);

    // Update the remaining data sectors.
    sectors_to_add -= indirect_alloc;
    current_sector += indirect_alloc;
  }

  //* 3. Handle doubly-indirect block allocation
  if (is_doubly_indirect_sector(current_sector) && (sectors_to_add != 0)) {
    block_sector_t* first_level = calloc(1, BLOCK_SECTOR_SIZE);
    block_sector_t* second_level = calloc(1, BLOCK_SECTOR_SIZE);
    if (!first_level || !second_level) {
      free(first_level);
      free(second_level);
      goto cleanup;
    }

    if (!is_doubly_indirect_sector(old_end_idx)) {
      // Allocate a new doubly indirect pointer.
      disk_inode->doubly_indirect = sector_list[sector_idx++];
    } else {
      // Read the existing doubly indirect pointer.
      read_from_cache(disk_inode->doubly_indirect, first_level, 0, BLOCK_SECTOR_SIZE);
    }

    // Calculate the range of first-level blocks we need to fill.
    const size_t start_level = doubly_indirect_first_level_index(current_sector);
    const size_t end_level = doubly_indirect_first_level_index((offset + size) / BLOCK_SECTOR_SIZE);

    for (size_t level = start_level; level <= end_level; ++level) {
      const size_t sl_offset = doubly_indirect_second_level_index(current_sector);
      const size_t sl_alloc = MIN(sectors_to_add, INDIRECT_POINTERS - sl_offset);
      if (sl_offset == 0) {
        // We are at the start of a new second-level block.
        // Then we need to allocate a new first-level block.
        first_level[level] = sector_list[sector_idx++];
      }

      // Write the second-level block.
      for (size_t i = 0; i < sl_alloc; ++i) {
        second_level[sl_offset + i] = sector_list[sector_idx++];
      }

      //! IMPORTANT: Becareful with the user_buffer here.
      write_to_cache(first_level[level], second_level + sl_offset,
                     sl_offset * sizeof(block_sector_t), sl_alloc * sizeof(block_sector_t));
      // Clear the second-level block for next use.
      memset(second_level, 0, BLOCK_SECTOR_SIZE);

      sectors_to_add -= sl_alloc;
      current_sector += sl_alloc;
    }
    write_to_cache(disk_inode->doubly_indirect, first_level, 0, BLOCK_SECTOR_SIZE);
    free(first_level);
    free(second_level);
  }

  ASSERT(sectors_to_add == 0); // All data sectors should be allocated.
  success = true;

cleanup:
  if (!success) {
    for (size_t i = 0; i < allocated; i++) {
      free_map_release(sector_list[i], 1);
    }
  }

  free(sector_list);
  return success;
}

/**
 * @brief Calculate the total number of disk sectors needed to store a file of given size.
 * 
 * This function computes the total sectors required, including direct, indirect, 
 * and doubly-indirect blocks based on the file's sector count. It considers the 
 * inode's multi-level indexing structure to determine the necessary metadata blocks.
 * 
 * @param sectors The number of data sectors the file occupies.
 * @return The total number of sectors needed (data + metadata blocks).
 */
static size_t calculate_required_sectors(size_t sectors) {
  // Handle edge case for zero sectors.
  if (sectors == 0) {
    return 0;
  }

  block_idx_t last_sector_idx = sectors - 1;
  size_t total_needed = 0;

  // Determine required sectors based on indexing level.
  if (is_direct_sector(last_sector_idx)) {
    total_needed = sectors;
  } else if (is_indirect_sector(last_sector_idx)) {
    total_needed = sectors + 1; // Add 1 indirect block.
  } else {
    // Doubly-indirect case.
    size_t remaining_data_sectors = sectors - MAX_SINGLE_INDIRECT_SIZE;
    size_t first_level_blocks = DIV_ROUND_UP(remaining_data_sectors, INDIRECT_POINTERS);
    // 1 indirect + 1 doubly-indirect + first-level blocks
    total_needed = sectors + 2 + first_level_blocks;
  }

  return total_needed;
}

/**
 * @brief Zero-initialize sectors within a file's byte range [start, end).
 * 
 * This function locates and zeros out the specified byte range across 
 * direct, indirect, and doubly-indirect sectors of a file's inode.
 * Handles partial sectors and ensures proper boundary checks.
 * 
 * @param disk_inode Pointer to the file's on-disk inode structure.
 * @param start Starting byte offset (inclusive) to zero-initialize.
 * @param end Ending byte offset (exclusive) to zero-initialize.
 * @return true if successful; false if memory allocation fails.
 */
static bool initialize_sectors_to_zeros(struct inode_disk* disk_inode, off_t start, off_t end) {
  static uint8_t zero_buffer[BLOCK_SECTOR_SIZE] = {0}; // Pre-initialized zero buffer.

  ASSERT(disk_inode != NULL);
  ASSERT(end >= start);

  if (start == end) {
    return true; // No bytes to initialize.
  }

  block_idx_t first_sector = start / BLOCK_SECTOR_SIZE;
  block_idx_t last_sector = end / BLOCK_SECTOR_SIZE;
  size_t sector_offset = start % BLOCK_SECTOR_SIZE;
  size_t bytes_remaining = end - start;

  block_sector_t* double_indirect_cache = NULL; // Cache for doubly-indirect blocks.

  for (block_idx_t sector_idx = first_sector; sector_idx <= last_sector; ++sector_idx) {
    block_sector_t target_sector;
    size_t buffer_offset = 0;
    size_t write_size = 0;

    // Locate the target sector (direct/indirect/doubly-indirect)
    if (is_direct_sector(sector_idx)) {
      target_sector = disk_inode->direct[sector_idx];
    } else if (is_indirect_sector(sector_idx)) {
      size_t indirect_offset = sector_idx - DIRECT_POINTERS;
      read_from_cache(disk_inode->indirect, &target_sector,
                      indirect_offset * sizeof(block_sector_t), sizeof(block_sector_t));
    } else {
      ASSERT(is_doubly_indirect_sector(sector_idx));

      // Cache first-level block on first access.
      if (double_indirect_cache == NULL) {
        double_indirect_cache = calloc(1, BLOCK_SECTOR_SIZE);
        if (double_indirect_cache == NULL) {
          return false;
        }
        read_from_cache(disk_inode->doubly_indirect, double_indirect_cache, 0, BLOCK_SECTOR_SIZE);
      }

      size_t first_level = doubly_indirect_first_level_index(sector_idx);
      size_t second_level = doubly_indirect_second_level_index(sector_idx);
      read_from_cache(double_indirect_cache[first_level], &target_sector,
                      second_level * sizeof(block_sector_t), sizeof(block_sector_t));
    }

    // Calculate write offset and size (handle partial sectors).
    if (sector_idx == first_sector) {
      buffer_offset = sector_offset;
      write_size = MIN(bytes_remaining, BLOCK_SECTOR_SIZE - sector_offset);
    } else {
      write_size = MIN(bytes_remaining, BLOCK_SECTOR_SIZE);
    }

    // Write zero data (leverage pre-initialized buffer for efficiency)
    write_to_cache(target_sector, zero_buffer, buffer_offset, write_size);
    bytes_remaining -= write_size;

    // Early termination (all bytes processed)
    if (bytes_remaining == 0) {
      break;
    }
  }

  // Clean up resources.
  free(double_indirect_cache);
  return true;
}

bool inode_isdir(const struct inode* inode) {
  ASSERT(inode != NULL);
  return inode->type == FILE_TYPE_DIR;
}

/**
 * @brief Increments the directory entry count in an inode and updates cache.
 * @param inode The inode of the directory.
 * @param num The number of entries to increment by.
 */
void increment_dir_entry_count(struct inode* inode, uint32_t num) {
  ASSERT(inode_isdir(inode));
  ASSERT(num != 0);

  inode->data.dir_entry_count += num;
  write_to_cache(inode->sector, &inode->data.dir_entry_count,
                 offsetof(struct inode_disk, dir_entry_count), sizeof(uint32_t));
}
