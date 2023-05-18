/**
 * @file cache.h
 * @brief Software cache library for a tiny web proxy.
 *
 * A software cache functions as key-value storage; saves some block of data
 * with its associated key such that future requests of the key return the
 * stored data.
 *
 * My implementation uses a circular doubly-linked list of cache blocks.
 * Key library details:
 *     - Request URIs used as keys.
 *     - Server response text used as values.
 *     - Block replacement via a least-recently-used policy (LRU).
 *
 * Descriptions of individual functions, data structures, and global variables
 * are provided in their respective leading comments.
 *
 * cache.c has more detailed implementation-related comments.
 *
 * @author Iltikin Wayet
 */

#include "csapp.h"
#include <stdbool.h>

// Max cache and object sizes
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

/**
 * @brief Cache block data structure.
 */
struct cache_block {
    ssize_t text_len;         // Length of the text within request header.
    ssize_t ref_cont;         // Reference count of the block.
    struct cache_block *next; // Pointer to next block in list.
    struct cache_block *prev; // Pointer to previous block in list.
    const char *uri; // Universal resource identifier of block (used as key).
    char *text;      // Request header text (Value in key value pair).
};
typedef struct cache_block cblock;

/**
 * @brief Cache data structure.
 *     Circular doubly-linked list saves space on tail pointer
 *     while allowing constant time tail access.
 *     Also just more fun than a regular list.
 */
struct cache_info {
    ssize_t size;  // Size of the cache (<= MAX_CACHE_SIZE).
    cblock *start; // Pointer to the starting block.
};
typedef struct cache_info cinfo;

// ---------- FUNCTION PROTOTYPES ---------- //

/**
 * @brief Initializes an empty cache.
 *     Cache empty iff: size = 0, start = NULL.
 */
void cache_init();

/**
 * @brief Writes the text of a cached server response.
 *
 * @param[in] uri : client request URI used as key.
 * @param[in] fd  : file descriptor to which block value is written.
 *
 * @return true if matching block in cache, false if not.
 */
bool cache_gettext(const char *uri, int fd);

/**
 * @brief Inserts a block into the cache.
 *
 * @param[in] uri      : client request URI used as key.
 * @param[in] text     : server response text to store with key.
 * @param[in] text_len : length of server response text.
 */
void cache_insert(const char *uri, char *text, ssize_t text_len);

/**
 * @brief Frees cache list and block items.
 */
void cache_free();

// ---------- HELPER PROTOTYPES ------------ //

/**
 * @brief Wrapper function for malloc routine.
 *     Cleaner look and handles errors.
 *
 * @param[in] size : amount of space to allocate in the heap.
 */
void *malloc_w(size_t size);
