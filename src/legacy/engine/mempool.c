//-----------------------------------------------------------------------------
//  mempool.c
//
//  A simple fast memory allocation package.
//
//  By Steve Hill in Graphics Gems III, David Kirk (ed.),
//    Academic Press, Boston, MA, 1992
//
//  Modified by L. Rossman, 8/13/94.
//
//  AllocInit()     - create an alloc pool, returns the old pool handle
//  Alloc()         - allocate memory
//  AllocReset()    - reset the current pool
//  AllocSetPool()  - set the current pool
//  AllocFree()     - free the memory used by the current pool.
//-----------------------------------------------------------------------------

/**
 * @file      mempool.c
 * @brief     Pooled memory allocation for legacy model objects.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#include <stdlib.h>
#include "mempool.h"

/*
**  ALLOC_BLOCK_SIZE - adjust this size to suit your installation - it
**  should be reasonably large otherwise you will be mallocing a lot.
*/

#define ALLOC_BLOCK_SIZE 64000 /*(62*1024)*/

/*
**  alloc_hdr_t - Header for each block of memory.
*/

typedef struct alloc_hdr_s {
    struct alloc_hdr_s* next; /* Next Block          */
    char *block,              /* Start of block      */
        *free,                /* Next free in block  */
        *end;                 /* block + block size  */
} alloc_hdr_t;

/*
**  alloc_root_t - Header for the whole pool.
*/

typedef struct alloc_root_s {
    alloc_hdr_t *first, /* First header in pool */
        *current;       /* Current header       */
} alloc_root_t;

/*
**  root - Pointer to the current pool.
*/

static alloc_root_t* root;

/*
**  AllocHdr()
**
**  Private routine to allocate a header and memory block.
*/

static alloc_hdr_t* AllocHdr(void);

/**
 * @brief Allocate one private 64000-byte pool block and its linked header.
 * @return Owned initialized header/block pair, or NULL if either allocation fails.
 * @note The legacy failure path does not release a successful companion
 * allocation when the other allocation fails; no stronger cleanup guarantee applies.
 */
static alloc_hdr_t* AllocHdr() {
    alloc_hdr_t* hdr;
    char* block;

    block = (char*)malloc(ALLOC_BLOCK_SIZE);
    hdr = (alloc_hdr_t*)malloc(sizeof(alloc_hdr_t));

    if (hdr == NULL || block == NULL)
        return (NULL);
    hdr->block = block;
    hdr->free = block;
    hdr->next = NULL;
    hdr->end = block + ALLOC_BLOCK_SIZE;

    return (hdr);
}

/*
**  AllocInit()
**
**  Create a new memory pool with one block.
**  Returns pointer to the new pool.
*/

/**
 * @brief Create a pool and select it as the shared current allocation pool.
 * @return Owned pool handle, or NULL if root/first-block allocation fails.
 * @post Replaces the shared root pointer; a successful pool is released by
 * selecting it and calling AllocFreePool(). Existing pools are not freed.
 * @note Failure may leave a partial root allocation; allocation calls are not thread-safe.
 */
alloc_handle_t* AllocInit() {
    alloc_handle_t* newpool;

    root = (alloc_root_t*)malloc(sizeof(alloc_root_t));
    if (root == NULL)
        return (NULL);
    if ((root->first = AllocHdr()) == NULL)
        return (NULL);
    root->current = root->first;
    newpool = (alloc_handle_t*)root;
    return (newpool);
}

/*
**  Alloc()
**
**  Use as a direct replacement for malloc().  Allocates
**  memory from the current pool.
*/

/**
 * @brief Reserve four-byte-aligned storage from the current pool.
 * @param[in] size Nonnegative byte count small enough to fit within one
 * ALLOC_BLOCK_SIZE block after four-byte rounding; oversized requests are unchecked.
 * @return Borrowed pointer into pool storage, or NULL if allocating another
 * block fails. Zero-byte requests need not return distinct pointers.
 * @pre AllocInit() succeeded and a live pool is current. Returned storage
 * must not be freed individually and is valid only until pool reset/free.
 * @note Rounds using the legacy 32-bit alignment mask; callers must not
 * request over-aligned C++ objects or sizes outside its representable range.
 */
char* Alloc(long size) {
    alloc_hdr_t* hdr = root->current;
    char* ptr;

    /*
    **  Align to 4 byte boundary - should be ok for most machines.
    **  Change this if your machine has weird alignment requirements.
    */
    size = (size + 3) & 0xfffffffc;

    ptr = hdr->free;
    hdr->free += size;

    /* Check if the current block is exhausted. */

    if (hdr->free >= hdr->end) {
        /* Is the next block already allocated? */

        if (hdr->next != NULL) {
            /* re-use block */
            hdr->next->free = hdr->next->block;
            root->current = hdr->next;
        } else {
            /* extend the pool with a new block */
            if ((hdr->next = AllocHdr()) == NULL)
                return (NULL);
            root->current = hdr->next;
        }

        /* set ptr to the first location in the next block */
        ptr = root->current->free;
        root->current->free += size;
    }

    /* Return pointer to allocated memory. */

    return (ptr);
}

/*
**  AllocSetPool()
**
**  Change the current pool.  Return the old pool.
*/

/**
 * @brief Replace the shared current-pool selection without freeing either pool.
 * @param[in] newpool Existing pool handle; NULL deselects a pool but cannot
 * be followed by allocation/reset/free until a live pool is selected.
 * @return Previous pool handle, possibly NULL. Ownership is unchanged.
 */
alloc_handle_t* AllocSetPool(alloc_handle_t* newpool) {
    alloc_handle_t* old = (alloc_handle_t*)root;
    root = (alloc_root_t*)newpool;
    return (old);
}

/*
**  AllocReset()
**
**  Reset the current pool for re-use.  No memory is freed,
**  so this is very fast.
*/

/**
 * @brief Rewind the current pool for reuse without releasing its blocks.
 * @pre A live pool is selected by AllocInit()/AllocSetPool().
 * @post Existing allocation contents may be overwritten by later Alloc()
 * calls; callers must discard outstanding object pointers before reuse.
 */
void AllocReset() {
    root->current = root->first;
    root->current->free = root->current->block;
}

/*
**  AllocFreePool()
**
**  Free the memory used by the current pool.
**  Don't use where AllocReset() could be used.
*/

/**
 * @brief Free every block/header and the root of the shared current pool.
 * @pre A live non-null pool is current and no outstanding allocations are in use.
 * @post All pointers into this pool become invalid and shared root becomes NULL.
 * Does not free any separately owned pools that are not currently selected.
 */
void AllocFreePool() {
    alloc_hdr_t *tmp, *hdr = root->first;

    while (hdr != NULL) {
        tmp = hdr->next;
        free((char*)hdr->block);
        free((char*)hdr);
        hdr = tmp;
    }
    free((char*)root);
    root = NULL;
}
