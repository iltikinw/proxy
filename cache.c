/**
 * @file cache.c
 * @brief Software cache library implementation for a tiny web proxy.
 *
 * A software cache functions as key-value storage; saves some block of data
 * with its associated key such that future requests of the key return the
 * stored data.
 *
 * My implementation uses a circular doubly-linked list of cache blocks.
 * Key implementation details:
 *     - Request URIs used as keys.
 *     - Server response text used as values.
 *     - Block replacement via a least-recently-used policy (LRU).
 *     - Cache automatically resizes by removing LRU block whenever necessary;
 *       stays under MAX_CACHE_SIZE in size.
 *     - Synchronization via mutexes and block reference counts.
 *
 * Descriptions of individual functions, data structures, and global variables
 * are provided in their respective leading comments.
 *
 * @author Iltikin Wayet
 */

#include "cache.h"
#include "csapp.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

// Mutex instance for the cache.
static pthread_mutex_t mutex;
// Cache instance.
static cinfo *cache;

// ---------- HELPER PROTOTYPES ------------ //
static void block_free(cblock *block);
static cblock *block_empty();
static cblock *block_new(const char *uri, char *text, size_t text_len);
static cblock *cache_findblock(const char *uri);
static void cache_addblock(cblock *block);
static void cache_remblock();

// ---------- FUNCTION ROUTINES ------------ //

/**
 * @brief Initializes an empty cache.
 *     Cache empty iff: size = 0, start = NULL.
 */
void cache_init() {
    cache = malloc_w(sizeof(cinfo));
    cache->size = 0;
    cache->start = NULL;
    pthread_mutex_init(&mutex, NULL);
}

/**
 * @brief Writes the text of a cached server response.
 *     Searches for block matching <uri>.
 *     If no such match, returns false.
 *     If match, moves block to start of the list.
 *         Then writes text to server file descriptor.
 *     Updates block reference count for synchronization.
 *
 * @param[in] uri : client request URI used as key.
 * @param[in] fd  : file descriptor to be used in server connection.
 *
 * @return true if matching block in cache, false if not.
 */
bool cache_gettext(const char *uri, int fd) {
    pthread_mutex_lock(&mutex);
    cblock *block = cache_findblock(uri);
    // If block not found, return.
    if (block == NULL) {
        pthread_mutex_unlock(&mutex);
        return false;
    } else {
        // Move block to start of list (LRU).
        if (block == cache->start) {
            block->ref_cont++;
        } else {
            block->next->prev = block->prev;
            block->prev->next = block->next;

            cache_addblock(block);
            cache->size -= block->text_len;
        }
        // Send text to client.
        pthread_mutex_unlock(&mutex);
        rio_writen(fd, block->text, block->text_len);

        // Block no longer referenced.
        pthread_mutex_lock(&mutex);
        block->ref_cont--;
        pthread_mutex_unlock(&mutex);
        return true;
    }
}

/**
 * @brief Inserts a block at the front of the cache.
 *     Automatically resizes cache as necessary.
 *     To do so, continuously removes blocks at end of the list (LRU).
 *
 * @param[in] uri      : client request URI used as key.
 * @param[in] text     : server response text to store with key.
 * @param[in] text_len : length of server response text.
 */
void cache_insert(const char *uri, char *text, ssize_t text_len) {
    pthread_mutex_lock(&mutex);
    cblock *block = cache_findblock(uri);
    // If matching block exists, do nothing.
    if (block == NULL) {
        // Downsize cache until block fits.
        while (cache->size + text_len > MAX_CACHE_SIZE) {
            cache_remblock();
        }
        block = block_new(uri, text, text_len);
        cache_addblock(block);
    }
    pthread_mutex_unlock(&mutex);
}

/**
 * @brief Frees cache list and block items.
 */
void cache_free() {
    if (cache->start != NULL) {
        cblock *curr = cache->start;
        do {
            curr = curr->next;
            block_free(curr->prev);
        } while (curr != cache->start);
    }
    free(cache);
}

// ---------- HELPER ROUTINES ------------ //

/**
 * @brief Frees a cache block.
 *     Waits until reference count of block = 0; no longer referenced.
 *     Then safely frees block URI, text, and data structure.
 *
 * @param[in] block : cache block to be freed.
 */
static void block_free(cblock *block) {
    // Continuously check if ref_cont 0.
    while (1) {
        if (block->ref_cont == 0) {
            free(block->uri);
            free(block->text);
            free(block);
            return;
        }
    }
}

/**
 * @brief Returns an empty block.
 *     Circular list; block points to itself.
 *
 * @return block with empty values.
 */
static cblock *block_empty() {
    cblock *block = malloc_w(sizeof(cblock));
    block->text_len = 0;
    block->ref_cont = 0;
    block->next = block;
    block->prev = block;
    block->uri = NULL;
    block->text = NULL;
    return block;
}

/**
 * @brief Returns a block with the inputted fields.
 *     First makes empty block, then initializes block values
 *     according to <uri>, <text>, and <text_len>.
 *
 * @param[in] uri      : client request URI used as key.
 * @param[in] text     : server response text to store with key.
 * @param[in] text_len : length of server response text.
 *
 * @return block with corresponding fields.
 */
static cblock *block_new(const char *uri, char *text, size_t text_len) {
    cblock *block = block_empty();
    block->text_len = text_len;
    size_t uri_len = strlen(uri) + 1;

    block->uri = malloc_w(sizeof(char) * uri_len);
    block->text = malloc_w(sizeof(char) * text_len);

    memcpy(block->uri, uri, sizeof(char) * uri_len);
    memcpy(block->text, text, sizeof(char) * text_len);

    return block;
}

/**
 * @brief Finds a block with matching <uri> in the cache.
 *
 * @param[in] uri : client request URI used as key.
 * @return NULL if list empty or no match, matching block otherwise.
 */
static cblock *cache_findblock(const char *uri) {
    cblock *curr = cache->start;
    // Check if list empty.
    if (curr == NULL) {
        return NULL;
    }
    // Iterate through and return matching block.
    do {
        if (!strcmp(uri, curr->uri))
            return curr;
        curr = curr->next;
    } while (curr != cache->start);
    return NULL;
}

/**
 * @brief Adds a block to the front of the list.
 *     Increments cache size accordingly.
 *
 * @param[in] block : block to add to front of the list.
 */
static void cache_addblock(cblock *block) {
    if (cache->start == NULL) {
        cache->start = block;
    } else {
        block->next = cache->start;
        block->prev = cache->start->prev;

        block->prev->next = block;
        block->next->prev = block;

        cache->start = block;
    }
    block->ref_cont++;
    cache->size += block->text_len;
}

/**
 * @brief Removes a block from the end of the list.
 *     Since LRU policy, the block we want to remove is always at the end
 *     of the circular, doubly-linked cache list.
 *     Updates cache size and block reference counts accordingly.
 */
static void cache_remblock() {
    cblock *rem = cache->start->prev;
    cblock *prev_new = rem->prev;

    prev_new->next = cache->start;
    cache->start->prev = prev_new;
    cache->size -= rem->text_len;

    if (rem == cache->start)
        cache->start = NULL;

    rem->ref_cont--;
    block_free(rem);
}

/**
 * @brief Wrapper function for malloc routine.
 *     Cleaner look and handles errors.
 *
 * @param[in] size : amount of space to allocate in the heap.
 */
void *malloc_w(size_t size) {
    void *ret = malloc(size);
    if (ret == NULL) {
        fprintf(stderr, "Memory error.\n");
        exit(1);
    }
    return ret;
}
