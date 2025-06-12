#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_POINTERS 120   /* Number of direct pointers in inode. */
#define INDIRECT_POINTERS 128 /* Number of pointers in an indirect block. */

#define MAX_TMP_SECTORS 130 /* Maximum number of sectors for temporary storage. */

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t direct[DIRECT_POINTERS]; /* Direct pointers to data sectors. */
  block_sector_t indirect;                /* Indirect pointer to data sectors. */
  block_sector_t doubly_indirect;         /* Doubly indirect pointer to data sectors. */
  off_t length;                           /* File size in bytes. */
  unsigned magic;                         /* Magic number. */
  uint32_t unused[4];                     /* Unused space for future expansion. */
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
  struct inode_disk data; /* Inode content. */
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);

  if (pos >= inode->data.length) {
    return -1;
  }

  size_t sector = pos / BLOCK_SECTOR_SIZE;

  if (sector < DIRECT_POINTERS) {
    /* Direct pointers. */
    return inode->data.direct[sector];
  }

  sector -= DIRECT_POINTERS;

  if (sector < INDIRECT_POINTERS) {
    /* Indirect pointers. */
    block_sector_t indirect_block;
    read_from_cache(inode->data.indirect, &indirect_block, sector * sizeof(block_sector_t),
                    sizeof(block_sector_t));
    return indirect_block;
  }

  // Doubly indirect pointers.
  sector -= INDIRECT_POINTERS;
  size_t first_level = sector / INDIRECT_POINTERS;
  size_t second_level = sector % INDIRECT_POINTERS;

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

  size_t sectors = bytes_to_sectors(length);
  block_sector_t* sector_list = malloc((sector + MAX_TMP_SECTORS) * sizeof(block_sector_t));

  if (sector_list == NULL) {
    free(disk_inode);
    return false;
  }

  // Initialize the inode structure.
  disk_inode->length = length;
  disk_inode->magic = INODE_MAGIC;

  size_t allocated = 0;          // Number of sectors allocated for the inode.
  size_t total_needed = 0;       // Total sectors needed for the inode.
  size_t remaining = 0;          // Remaining sectors after direct and indirect pointers.
  size_t first_level_blocks = 0; // Number of first-level blocks for doubly indirect pointers.

  if (sectors <= DIRECT_POINTERS) {
    total_needed = sectors;
  } else if (sectors <= (DIRECT_POINTERS + INDIRECT_POINTERS)) {
    total_needed = sectors + 1;
  } else {
    remaining = sectors - DIRECT_POINTERS - INDIRECT_POINTERS;
    first_level_blocks = DIV_ROUND_UP(remaining, INDIRECT_POINTERS);
    total_needed = sectors + 2 + first_level_blocks;
  }

  bool success = false; // Indicates if the inode creation was successful.

  // Alloc all sectors needed for the inode.
  for (size_t i = 0; i < total_needed; ++i, ++allocated) {
    if (!free_map_allocate(1, &sector_list[allocated])) {
      goto cleanup;
    }
  }

  size_t sector_index = 0;

  // Fill in the direct pointers.
  for (size_t i = 0; i < DIRECT_POINTERS && i < sectors; ++i) {
    disk_inode->direct[i] = sector_list[sector_index++];
  }

  // Fill in the indirect pointer if needed.
  if (sectors > DIRECT_POINTERS) {
    disk_inode->indirect = sector_list[sector_index++];
    block_sector_t* indirect_block = calloc(1, BLOCK_SECTOR_SIZE);
    if (indirect_block == NULL) {
      goto cleanup;
    }
    size_t indirect_sectors = MIN(sectors - DIRECT_POINTERS, INDIRECT_POINTERS);
    for (size_t i = 0; i < indirect_sectors; i++) {
      indirect_block[i] = sector_list[sector_index++];
    }
    write_to_cache(disk_inode->indirect, indirect_block, 0, BLOCK_SECTOR_SIZE);
    free(indirect_block);
  }

  // Fill in the doubly indirect pointer if needed.
  if (sectors > (DIRECT_POINTERS + INDIRECT_POINTERS)) {
    disk_inode->doubly_indirect = sector_list[sector_index++];
    ASSERT(remaining != 0 && first_level_blocks != 0);
    block_sector_t* first_level_block = calloc(1, BLOCK_SECTOR_SIZE);
    if (first_level_block == NULL) {
      goto cleanup;
    }
    block_sector_t* second_level_block = calloc(1, BLOCK_SECTOR_SIZE);
    if (second_level_block == NULL) {
      free(first_level_block);
      goto cleanup;
    }

    for (size_t i = 0; i < first_level_blocks; i++) {
      block_sector_t first_level_sector = sector_list[sector_index++];
      first_level_block[i] = first_level_sector;
      size_t data_sectors_in_this_block = MIN(INDIRECT_POINTERS, remaining - i * INDIRECT_POINTERS);
      for (size_t j = 0; j < data_sectors_in_this_block; j++) {
        second_level_block[j] = sector_list[sector_index++];
      }
      write_to_cache(first_level_sector, second_level_block, 0, BLOCK_SECTOR_SIZE);
      memset(second_level_block, 0,
             BLOCK_SECTOR_SIZE); // Clear the second level block for next use.
    }
    write_to_cache(disk_inode->doubly_indirect, first_level_block, 0, BLOCK_SECTOR_SIZE);
    free(first_level_block);
    free(second_level_block);
  }

  // Initialize all data sectors to zero.
  static uint8_t zeros[BLOCK_SECTOR_SIZE];

  for (size_t i = 0; i < sectors; ++i) {
    block_sector_t data_sector;
    if (i < DIRECT_POINTERS) {
      data_sector = disk_inode->direct[i];
    } else if (i < (DIRECT_POINTERS + INDIRECT_POINTERS)) {
      size_t offset = i - DIRECT_POINTERS;
      read_from_cache(disk_inode->indirect, &data_sector, offset * sizeof(block_sector_t),
                      sizeof(block_sector_t));
    } else {
      size_t offest = i - (DIRECT_POINTERS + INDIRECT_POINTERS);
      size_t first_level = offest / INDIRECT_POINTERS;
      size_t second_level = offest % INDIRECT_POINTERS;
      block_sector_t first_level_sector;
      // Read the first level sector from the doubly indirect block.
      read_from_cache(disk_inode->doubly_indirect, &first_level_sector,
                      first_level * sizeof(block_sector_t), sizeof(block_sector_t));
      // Read the second level sector from the first level block.
      read_from_cache(first_level_sector, &data_sector, second_level * sizeof(block_sector_t),
                      sizeof(block_sector_t));
    }

    write_to_cache(data_sector, zeros, 0, BLOCK_SECTOR_SIZE);
  }

  // Finally, write the inode to disk.
  write_to_cache(sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
  success = true;

cleanup:
  if (!success) {
    for (size_t i = 0; i < allocated; i++) {
      free_map_release(sector_list[i], 1);
    }
  }
  free(disk_inode);
  free(sector_list);
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
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
  read_from_cache(inode->sector, &inode->data, 0, BLOCK_SECTOR_SIZE);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL)
    inode->open_cnt++;
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

  if (--inode->open_cnt > 0)
    return;

  list_remove(&inode->elem);

  if (!inode->removed) {
    free(inode);
    return;
  }

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
    block_sector_t sector_idx = byte_to_sector(inode, offset);
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

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    // TODO: Handle the case where sector_idx is -1 (not found).
    ASSERT(sector_idx != (block_sector_t)-1);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
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
