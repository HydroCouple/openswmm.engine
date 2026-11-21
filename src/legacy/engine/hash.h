//-----------------------------------------------------------------------------
//   hash.h
//
//   Header file for Hash Table module hash.c.
//-----------------------------------------------------------------------------

/**
 * @file      hash.h
 * @brief     Hash tables used to resolve legacy model identifiers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef HASH_H
#define HASH_H

#define HTMAXSIZE 1999
#define NOTFOUND -1

struct HTentry {
    char* key;
    int data;
    struct HTentry* next;
};

typedef struct HTentry* HTtable;

HTtable* HTcreate(void);
int HTinsert(HTtable*, char*, int);
int HTfind(HTtable*, const char*);
char* HTfindKey(HTtable*, const char*);
void HTfree(HTtable*);

#endif //HASH_H
