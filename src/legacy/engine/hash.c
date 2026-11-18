//-----------------------------------------------------------------------------
//   hash.c
//
//   Implementation of a simple Hash Table for string storage & retrieval
//   CASE INSENSITIVE
//
//   Written by L. Rossman
//   Last Updated on 6/19/03
//
//   The hash table data structure (HTable) is defined in "hash.h".
//   Interface Functions:
//      HTcreate() - creates a hash table
//      HTinsert() - inserts a string & its index value into a hash table
//      HTfind()   - retrieves the index value of a string from a table
//      HTfree()   - frees a hash table
//-----------------------------------------------------------------------------

/**
 * @file      hash.c
 * @brief     Hash tables used to resolve legacy model identifiers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#include <stdlib.h>
#include <string.h>
#include "hash.h"
#define UCHAR(x) (((x) >= 'a' && (x) <= 'z') ? ((x) & ~32) : (x))

/* Case-insensitive comparison of strings s1 and s2 */
/**
 * @brief Compare two nonempty ASCII identifiers without case sensitivity.
 * @param[in] s1 Non-null borrowed null-terminated first string.
 * @param[in] s2 Non-null borrowed null-terminated second string.
 * @return One for equal strings after ASCII case folding, zero otherwise.
 * @pre Both identifiers are nonempty; the loop reads one character beyond
 * a matching pair before its termination check. No storage is retained.
 */
int samestr(const char* s1, const char* s2) {
    int i;
    for (i = 0; UCHAR(s1[i]) == UCHAR(s2[i]); i++)
        if (!s1[i + 1] && !s2[i + 1])
            return (1);
    return (0);
} /*  End of samestr  */

/* Use Fletcher's checksum to compute 2-byte hash of string */
/**
 * @brief Map an ASCII identifier to a case-insensitive Fletcher-checksum bucket.
 * @param[in] str Non-null borrowed null-terminated identifier; empty is permitted.
 * @return Bucket index in [0, HTMAXSIZE - 1]. Input storage is unchanged.
 */
unsigned int hash(const char* str) {
    unsigned int sum1 = 0, check1;
    unsigned long sum2 = 0L;
    while ('\0' != *str) {
        sum1 += UCHAR(*str);
        str++;
        if (255 <= sum1)
            sum1 -= 255;
        sum2 += sum1;
    }
    check1 = sum2;
    check1 %= 255;
    check1 = 255 - (sum1 + check1) % 255;
    sum1 = 255 - (sum1 + check1) % 255;
    return (((check1 << 8) | sum1) % HTMAXSIZE);
}

/**
 * @brief Allocate an empty case-insensitive identifier table.
 * @return Owned zero-initialized HTMAXSIZE bucket array, or NULL on allocation failure.
 * @note Release the table and its entries with HTfree(); key strings remain externally owned.
 */
HTtable* HTcreate() {
    int i;
    HTtable* ht = (HTtable*)calloc(HTMAXSIZE, sizeof(HTtable));
    if (ht != NULL)
        for (i = 0; i < HTMAXSIZE; i++)
            ht[i] = NULL;
    return (ht);
}

/**
 * @brief Prepend a key/data entry to a hash table without copying its key.
 * @param[in,out] ht Non-null table created by HTcreate().
 * @param[in] key Borrowed nonempty null-terminated identifier; it must remain
 * alive and unchanged for the entry's lifetime.
 * @param[in] data Integer value associated with the identifier.
 * @return One on insertion, zero on bucket/allocation failure.
 * @note Duplicate keys are allowed and newest entries are found first. The
 * table owns the entry allocation, but never frees key storage.
 */
int HTinsert(HTtable* ht, char* key, int data) {
    unsigned int i = hash(key);
    struct HTentry* entry;
    if (i >= HTMAXSIZE)
        return (0);
    entry = (struct HTentry*)malloc(sizeof(struct HTentry));
    if (entry == NULL)
        return (0);
    entry->key = key;
    entry->data = data;
    entry->next = ht[i];
    ht[i] = entry;
    return (1);
}

/**
 * @brief Look up the most recently inserted case-insensitive key match.
 * @param[in] ht Non-null borrowed table created by HTcreate().
 * @param[in] key Non-null borrowed nonempty null-terminated identifier.
 * @return Stored integer value, or NOTFOUND if no entry matches.
 * @note No storage is modified or retained; returned values can coincide
 * with NOTFOUND if the caller inserted that sentinel as data.
 */
int HTfind(HTtable* ht, const char* key) {
    unsigned int i = hash(key);
    struct HTentry* entry;
    if (i >= HTMAXSIZE)
        return (NOTFOUND);
    entry = ht[i];
    while (entry != NULL) {
        if (samestr(entry->key, key))
            return (entry->data);
        entry = entry->next;
    }
    return (NOTFOUND);
}

/**
 * @brief Retrieve the original stored spelling of a case-insensitive key.
 * @param[in] ht Non-null borrowed table created by HTcreate().
 * @param[in] key Non-null borrowed nonempty null-terminated lookup identifier.
 * @return Borrowed original key pointer, or NULL if no entry matches.
 * @note The pointer's lifetime follows the external key owner; the table does
 * not copy, free or grant mutation rights to the stored identifier.
 */
char* HTfindKey(HTtable* ht, const char* key) {
    unsigned int i = hash(key);
    struct HTentry* entry;
    if (i >= HTMAXSIZE)
        return (NULL);
    entry = ht[i];
    while (entry != NULL) {
        if (samestr(entry->key, key))
            return (entry->key);
        entry = entry->next;
    }
    return (NULL);
}

/**
 * @brief Free all hash entries and the table bucket array.
 * @param[in] ht Non-null exclusively owned table created by HTcreate().
 * @post Table and entry pointers are invalid; borrowed key strings are not freed.
 * The caller must discard or reset its own ht pointer after this call.
 */
void HTfree(HTtable* ht) {
    struct HTentry *entry, *nextentry;
    int i;
    for (i = 0; i < HTMAXSIZE; i++) {
        entry = ht[i];
        while (entry != NULL) {
            nextentry = entry->next;
            free(entry);
            entry = nextentry;
        }
    }
    free(ht);
}
