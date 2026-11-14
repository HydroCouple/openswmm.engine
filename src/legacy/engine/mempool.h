//-----------------------------------------------------------------------------
//  mempool.h
//
//  Header for mempool.c
//
//  The type alloc_handle_t provides an opaque reference to the
//  alloc pool - only the alloc routines know its structure.
//-----------------------------------------------------------------------------

/**
 * @file      mempool.h
 * @brief     Pooled memory allocation for legacy model objects.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef MEMPOOL_H
#define MEMPOOL_H

typedef struct {
    long dummy;
} alloc_handle_t;

alloc_handle_t* AllocInit(void);
char* Alloc(long);
alloc_handle_t* AllocSetPool(alloc_handle_t*);
void AllocReset(void);
void AllocFreePool(void);

#endif //MEMPOOL_H
