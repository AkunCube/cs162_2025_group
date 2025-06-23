#include <debug.h>
#include <string.h>
#include <stdbool.h>
#include "filesys/cache.h"
#include "threads/synch.h"
#include "filesys/off_t.h"
#include "devices/block.h"
#include "filesys/filesys.h"
#include "threads/thread.h"

/// File Cache implementation

#define MAX_FILE_CACHES 64 /// Maximum number of cache slots (adjust based on system memory)

#define CACHE_FREE 0x1           // Cache slot is unused and available for allocation
#define CACHE_VALID 0x2          // Cache slot contains valid data for a sector
#define CACHE_DIRTY 0x4          // Cache data has been modified (needs write-back to disk)
#define CACHE_REFERENCED 0x8     // Cache has been recently accessed (for clock algorithm)
#define CACHE_LOCKED 0x10        // Cache is currently locked for exclusive access
#define CACHE_BEING_EVICTED 0x20 // Cache is in the process of being evicted (do not access)

// Condition variable to wait when all caches are being evicted
static struct condition all_evicted;

/// Cache slot structure for holding sector data and metadata
typedef struct {
  uint8_t data[BLOCK_SECTOR_SIZE]; // Data buffer for a disk sector
  block_sector_t sector_id;        // Sector number (invalid when -1)
  uint8_t status_flags;            // Bitmask of cache status flags (see defines above)
  struct condition access_cond;    // Condition variable for per-slot synchronization
} CacheSlot;

/// File cache manager structure
typedef struct {
  struct lock global_lock;                // Global lock for cache system (protect all operations)
  size_t clock_hand_pos;                  // Pointer for clock replacement algorithm (0-based index)
  CacheSlot cache_slots[MAX_FILE_CACHES]; // Array of cache slots
} FileCaches;

static FileCaches fc; // Global cache manager instance

static CacheSlot* find_free_cache(FileCaches* fc);
static CacheSlot* find_cache_by_sector(FileCaches* fc, block_sector_t sector);
static CacheSlot* evict_cache_with_clock(FileCaches* fc);
typedef CacheSlot*(evict_func)(FileCaches* fc);

static evict_func* evict_strategy = evict_cache_with_clock;

/**
 * @brief Initialize the file buffer cache system
 * 
 * Sets up the entire cache infrastructure, including:
 * 1. Initializing the global cache lock and eviction condition variable
 * 2. Marking all cache slots as FREE and invalid
 * 3. Initializing per-slot condition variables for synchronization
 * 
 * This function must be called exactly once before any cache operations
 * (read/write/lookup) to ensure proper synchronization and state management.
 */
void init_file_caches(void) {
  lock_init(&fc.global_lock);
  cond_init(&all_evicted);

  for (int i = 0; i < MAX_FILE_CACHES; i++) {
    fc.cache_slots[i].status_flags = CACHE_FREE;
    fc.cache_slots[i].sector_id = (block_sector_t)-1; // Invalid sector number
    cond_init(&fc.cache_slots[i].access_cond);
  }

  // Set initial clock hand position for eviction algorithm
  fc.clock_hand_pos = 0;
}

/**
 * @brief Find and lock a cache slot by sector number
 * 
 * Searches the cache for a valid entry containing the specified sector.
 * If found, locks the cache slot to prevent concurrent access and returns it.
 * If the slot is currently locked by another thread, the caller will block
 * until it becomes available.
 * 
 * @param fc     Pointer to the file caches structure
 * @param sector Sector number to locate
 * @return Pointer to the locked cache slot if found, NULL otherwise
 */
static CacheSlot* find_cache_by_sector(FileCaches* fc, block_sector_t sector) {
  ASSERT(lock_held_by_current_thread(&fc->global_lock));

  for (int i = 0; i < MAX_FILE_CACHES; i++) {
    if ((fc->cache_slots[i].status_flags & (CACHE_FREE | CACHE_BEING_EVICTED))) {
      continue; // Skip free caches or caches being evicted.
    }

    if ((fc->cache_slots[i].status_flags & CACHE_VALID) && fc->cache_slots[i].sector_id == sector) {
      if (fc->cache_slots[i].status_flags & CACHE_LOCKED) {
        // Since this is not a preemptive os kernel, by the time we find the cache,
        // the cache is still unevicted, so we can safely read or write to it.
        cond_wait(&fc->cache_slots[i].access_cond, &fc->global_lock);
        // Ensure the cache is not locked.
        ASSERT(!(fc->cache_slots[i].status_flags & CACHE_LOCKED));
      }
      // Lock the cache for exclusive access.
      fc->cache_slots[i].status_flags |= CACHE_LOCKED;
      return &fc->cache_slots[i];
    }
  }

  return NULL;
}

/**
 * @brief Locate and initialize a free cache slot
 * 
 * Scans the cache array for the first available slot marked as FREE.
 * Upon finding a free slot, resets its state, locks it for exclusive access,
 * and marks it as valid for immediate use.
 * 
 * @param fc Pointer to the file caches structure
 * @return Pointer to the initialized cache slot if found, NULL if all slots are active
 */
static CacheSlot* find_free_cache(FileCaches* fc) {
  ASSERT(lock_held_by_current_thread(&fc->global_lock));

  for (int i = 0; i < MAX_FILE_CACHES; i++) {
    if (fc->cache_slots[i].status_flags & CACHE_FREE) {
      fc->cache_slots[i].status_flags = 0; // Reset all flags
      // Lock the cache for exclusive access and set it as valid.
      fc->cache_slots[i].status_flags |= (CACHE_LOCKED | CACHE_VALID);
      return &fc->cache_slots[i];
    }
  }
  return NULL;
}

/**
 * @brief Evict a cache slot using a four-round clock replacement algorithm
 * 
 * This function implements a four-phase clock algorithm to select a cache slot for eviction:
 * 1. **First Round**: Search for an unreferenced and unmodified (clean) cache slot
 * 2. **Second Round**: Clear all reference bits and prioritize dirty slots for eviction
 * 3. **Third Round**: Re-check for unreferenced/clean slots after clearing references
 * 4. **Fourth Round**: Evict any remaining dirty slot if no clean slots are found
 * 
 * If no evictable slot is found after four rounds, the function waits on a condition 
 * variable until another thread signals that a slot has been evicted.
 * 
 * @param fc Pointer to the file cache manager structure
 * @return Pointer to the evicted cache slot (guaranteed non-NULL by assertion)
 */
static CacheSlot* evict_cache_with_clock(FileCaches* fc) {
  ASSERT(fc != NULL);
  ASSERT(lock_held_by_current_thread(&fc->global_lock));
  //! IMPORTANT: All cache slots must be valid and non-free at this point.

  CacheSlot *evicted_cache, *cache;
  size_t clock_hand;

  // Four round implementation of the clock algorithm.
  while (true) {
    const size_t old_clock_hand = fc->clock_hand_pos;
    clock_hand = old_clock_hand;

    evicted_cache = NULL;

    // 1. First round.
    do {
      cache = &fc->cache_slots[clock_hand];
      clock_hand = (clock_hand + 1) % MAX_FILE_CACHES;
      if (cache->status_flags & CACHE_BEING_EVICTED) {
        // ? Shoule we changed to a cyclic double linked list?
        // ? So that we don't need to check every time if the cache is being evicted.
        // Skip any cache that is currently being evicted.
        // Same code below.
        continue;
      }
      if (!(cache->status_flags & (CACHE_REFERENCED | CACHE_DIRTY))) {
        evicted_cache = cache;
        goto FIND;
      }
    } while (clock_hand != old_clock_hand);

    // 2. Second round.
    do {
      cache = &fc->cache_slots[clock_hand];
      clock_hand = (clock_hand + 1) % MAX_FILE_CACHES;
      if (cache->status_flags & CACHE_BEING_EVICTED) {
        continue;
      }
      if (cache->status_flags & CACHE_DIRTY) {
        evicted_cache = cache;
        goto FIND;
      }
      cache->status_flags &= ~CACHE_REFERENCED; // Clear the referenced flag
    } while (clock_hand != old_clock_hand);

    // 3. Third round.
    do {
      cache = &fc->cache_slots[clock_hand];
      clock_hand = (clock_hand + 1) % MAX_FILE_CACHES;
      if (cache->status_flags & CACHE_BEING_EVICTED) {
        continue;
      }
      if (!(cache->status_flags & (CACHE_REFERENCED | CACHE_DIRTY))) {
        evicted_cache = cache;
        goto FIND;
      }
    } while (clock_hand != old_clock_hand);

    // 4. Fourth round.
    do {
      cache = &fc->cache_slots[clock_hand];
      clock_hand = (clock_hand + 1) % MAX_FILE_CACHES;
      if (cache->status_flags & CACHE_BEING_EVICTED) {
        continue;
      }
      if (cache->status_flags & CACHE_DIRTY) {
        evicted_cache = cache;
        goto FIND;
      }
    } while (clock_hand != old_clock_hand);

    /*
    ** No evictable cache slot found after four passes.
    ** This is an edge case that occurs when all cache slots are currently marked as CACHE_BEING_EVICTED.
    ** To resolve this, we wait on the 'all_evicted' condition variable,
    ** which is signaled by any thread that completes evicting a cache slot
    ** After waking up, we restart the eviction process from the top,
    ** as another thread may have freed up a cache slot while we waited.
    **/
    cond_wait(&all_evicted, &fc->global_lock);
    // Recheck all cache slots after being signaled
  }

FIND:
  ASSERT(evicted_cache != NULL);
  ASSERT(fc->clock_hand_pos != clock_hand);
  fc->clock_hand_pos = clock_hand;
  evicted_cache->status_flags |= CACHE_BEING_EVICTED; // Mark the cache as being evicted.

  if (evicted_cache->status_flags & CACHE_LOCKED) {
    // If the cache is locked, wait for it to be unlocked.
    cond_wait(&evicted_cache->access_cond, &fc->global_lock);
    ASSERT(!(evicted_cache->status_flags & CACHE_LOCKED));
    evicted_cache->status_flags |= CACHE_LOCKED; // Lock the cache for exclusive access
  }

  // If the evicted cache is dirty, write it back to disk.
  if (evicted_cache->status_flags & CACHE_DIRTY) {
    lock_release(&fc->global_lock); // Release the lock before writing.
    block_write(fs_device, evicted_cache->sector_id, evicted_cache->data);
    lock_acquire(&fc->global_lock); // Reacquire the lock after writing.
  }

  evicted_cache->status_flags &= ~(CACHE_DIRTY | CACHE_REFERENCED);
  return evicted_cache;
}

/**
 * @brief Read data from the file cache with on-demand fetching
 * 
 * Reads data from the cache if present; otherwise fetches from disk,
 * handles cache allocation/eviction, and maintains cache state.
 * 
 * Process Flow:
 * 1. Checks cache for the requested sector
 * 2. On cache hit: copies data to user buffer, marks cache as referenced
 * 3. On cache miss:
 *    - Allocates a free cache slot or evicts using clock algorithm
 *    - Reads sector data from disk into the new cache slot
 *    - Copies data to user buffer and updates cache state
 * 
 * @param sector       Sector number to read
 * @param user_buffer  Destination buffer for read data
 * @param sector_off   Offset within the sector to read from
 * @param size         Number of bytes to read (0 < size <= BLOCK_SECTOR_SIZE)
 * 
 * @note Disk I/O operations are performed while the global lock is released
 *       to maximize concurrency. Cache state is carefully managed to prevent
 *       race conditions during eviction and updates.
 */
void read_from_cache(block_sector_t sector, void* user_buffer, off_t sector_off, size_t size) {
  ASSERT(user_buffer != NULL);
  ASSERT(size <= BLOCK_SECTOR_SIZE);
  ASSERT(sector_off + size <= BLOCK_SECTOR_SIZE); // Ensure no out-of-bounds read

  lock_acquire(&fc.global_lock);

  // 1. Check for cache hit.
  CacheSlot* cache = find_cache_by_sector(&fc, sector);
  if (cache) {
    // Cache hit: update data and mark referenced.
    ASSERT(cache->status_flags & (CACHE_VALID | CACHE_LOCKED));
    ASSERT(cache->sector_id == sector);
    memcpy(user_buffer, cache->data + sector_off, size);
    cache->status_flags |= (CACHE_REFERENCED);
    cache->status_flags &= ~CACHE_LOCKED;
    cond_signal(&cache->access_cond, &fc.global_lock);
    lock_release(&fc.global_lock);
    return;
  }

  // 2. Cache miss: allocate or evict.
  cache = find_free_cache(&fc);
  if (cache == NULL) {
    cache = evict_strategy(&fc);
    ASSERT(cache != NULL);
  }

  ASSERT(lock_held_by_current_thread(&fc.global_lock));
  ASSERT(cache->status_flags & (CACHE_VALID | CACHE_LOCKED));

  // 3. Initialize new cache slot
  cache->sector_id = sector; // Set the sector number for the cache ASAP.
  lock_release(&fc.global_lock);

  block_read(fs_device, sector, cache->data);

  // 4. Read user data into cache.
  lock_acquire(&fc.global_lock);
  memcpy(user_buffer, cache->data + sector_off, size);

  // 5. Finalize cache state.
  cache->status_flags |= CACHE_REFERENCED;
  cache->status_flags &=
      ~(CACHE_LOCKED | CACHE_BEING_EVICTED);         // Clear the locked and being evicted flags.
  cond_signal(&all_evicted, &fc.global_lock);        // Signal that a cache has been evicted
  cond_signal(&cache->access_cond, &fc.global_lock); // Wake up any waiting threads
  lock_release(&fc.global_lock);
}

/**
 * @brief Write data to the file cache with write-back policy
 * 
 * This function handles both cache hits and misses when writing data:
 * 1. For cache hits, directly updates the cached sector and marks it dirty
 * 2. For cache misses:
 *    - Allocates a new cache slot if available
 *    - Evicts an existing slot using the clock algorithm if necessary
 *    - Reads the original sector data from disk for partial writes
 * 3. Manages cache locking, dirty flags, and eviction signaling automatically
 * 
 * @param sector       Sector number to write
 * @param user_buffer  Source buffer containing data to write
 * @param sector_off   Offset within the sector to start writing
 * @param size         Number of bytes to write (must satisfy: size ≤ BLOCK_SECTOR_SIZE)
 * 
 * @note The global cache lock is only held during metadata operations.
 *       Disk I/O operations are performed after releasing the lock to minimize contention.
 */
void write_to_cache(block_sector_t sector, const void* user_buffer, off_t sector_off, size_t size) {
  ASSERT(user_buffer != NULL);
  ASSERT(size <= BLOCK_SECTOR_SIZE);
  ASSERT(sector_off + size <= BLOCK_SECTOR_SIZE); // Ensure no out-of-bounds read

  lock_acquire(&fc.global_lock);

  // 1. Check for cache hit.
  CacheSlot* cache = find_cache_by_sector(&fc, sector);
  if (cache) {
    // Cache hit: update data and mark dirty.
    ASSERT(cache->status_flags & (CACHE_VALID | CACHE_LOCKED));
    ASSERT(cache->sector_id == sector);
    memcpy(cache->data + sector_off, user_buffer, size);
    cache->status_flags |= CACHE_DIRTY;
    cache->status_flags &= ~CACHE_LOCKED;
    cond_signal(&cache->access_cond, &fc.global_lock);
    lock_release(&fc.global_lock);
    return;
  }

  // 2. Cache miss: allocate or evict.
  cache = find_free_cache(&fc);
  if (cache == NULL) {
    cache = evict_strategy(&fc);
    ASSERT(cache != NULL);
  }

  ASSERT(lock_held_by_current_thread(&fc.global_lock));
  ASSERT(cache->status_flags & (CACHE_VALID | CACHE_LOCKED));

  // 3. Initialize new cache slot.
  cache->sector_id = sector;

  // 4. Handle partial writes (read original data first).
  bool needs_read = (sector_off != 0) || (size != BLOCK_SECTOR_SIZE);
  if (needs_read) {
    lock_release(&fc.global_lock); // Release lock during disk I/O
    block_read(fs_device, sector, cache->data);
    lock_acquire(&fc.global_lock);
  }

  // 5. Write user data into cache.
  memcpy(cache->data + sector_off, user_buffer, size);

  // 6. Finalize cache state.
  cache->status_flags |= CACHE_DIRTY;
  cache->status_flags &=
      ~(CACHE_LOCKED | CACHE_BEING_EVICTED);         // Clear the locked and being evicted flags.
  cond_signal(&all_evicted, &fc.global_lock);        // Signal that a cache has been evicted
  cond_signal(&cache->access_cond, &fc.global_lock); // Wake up any waiting threads
  lock_release(&fc.global_lock);
}

/**
 * @brief Flush all dirty cache blocks to storage and clear dirty flags
 * 
 * This function iterates through all cache slots in the file cache pool,
 * writing any cache blocks marked as "dirty" (CACHE_DIRTY) to disk to ensure
 * modifications in memory are persisted. After writing, it clears the dirty flag
 * for each block. Thread safety is ensured via a global lock.
 */
void save_all_caches_to_storage() {
  lock_acquire(&fc.global_lock);

  for (int i = 0; i < MAX_FILE_CACHES; i++) {
    CacheSlot* cache = &fc.cache_slots[i];
    if (cache->status_flags & CACHE_DIRTY) {
      ASSERT(cache->status_flags & CACHE_VALID);
      // Write dirty caches back to disk.
      block_write(fs_device, cache->sector_id, cache->data);
      cache->status_flags &= ~CACHE_DIRTY; // Clear dirty flag after writing.
    }
  }

  lock_release(&fc.global_lock);
}
