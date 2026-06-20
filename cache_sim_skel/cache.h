/*************************************************************
 * cache data structure header for programming assignment #2
 *************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include "util.h"

typedef struct {
    unsigned long int num_of_total_accesses;
    unsigned long int num_of_hits;
    unsigned long int num_of_total_misses;
    unsigned long int num_of_compulsory_misses;
    unsigned long int num_of_capacity_misses;
    unsigned long int num_of_conflict_misses;
    long double hit_rate;
} CacheStats;


typedef struct {
  bool valid;           /* Valid */
  BitArray * tag;       /* Tag */
  unsigned char * data; /* Data */
  unsigned long last_used;
  unsigned long insertion_time;
  unsigned long access_count;
} CacheLine;

typedef struct {
  CacheLine * lines;    /* n-way: there are n lines */
} CacheSet;

typedef struct {
  unsigned int addr_width;   /* address bit-width
                                e.g., 16 -> 16-bit address */
  unsigned int line_size;    /* cache block (line) size in power of two bytes,
                                e.g., 4 -> 2^4 = 16 B */
  unsigned int mem_size;     /* primary memory size in power of two bytes,
                                 e.g., 20 -> 2^20 B = 1 MB */
  unsigned int cache_size;   /* cache size in power of two bytes 
                                 e.g., 10 -> 2^10 B = 1 KB */
  unsigned int assoc;        /* set associativity 
                                0: fully-associative,
                                1: direct-mapped,
                                any positive number n, power of 2 : n-way ,
                                 otherwise: error */
  unsigned int policy;       /* replacement policy 
                                0: random,
                                1: LRU,
                                2: optimal,
                                otherwise: error  */
  unsigned int num_lines;
  unsigned int num_sets;
  unsigned int num_ways;
  unsigned int set_index_bits;
  unsigned int tag_size;

  CacheSet * sets;    /* sets */
  CacheStats * stats; /* statistics */
} Cache;

Cache* initCache(unsigned int addr_width, unsigned int line_size, unsigned int mem_size, unsigned int cache_size, unsigned int assoc, unsigned int policy);
void freeCache(Cache* cache);
void print_cache_config(Cache * cache);
void print_stats(CacheStats * stat);
