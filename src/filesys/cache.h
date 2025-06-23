#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "filesys/off_t.h"
#include "devices/block.h"

void init_file_caches(void);
void read_from_cache(block_sector_t sector, void* user_buffer, off_t sector_off, size_t size);
void write_to_cache(block_sector_t sector, const void* user_buffer, off_t sector_off, size_t size);
void save_all_caches_to_storage(void);

#endif /* filesys/cache.h */
