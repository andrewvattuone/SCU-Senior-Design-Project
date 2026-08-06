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

typedef enum {
    MESI_INVALID = 0,
    MESI_SHARED,
    MESI_EXCLUSIVE,
    MESI_MODIFIED
} MESIState;

typedef enum {
  RL_DISABLED = 0,
  RL_STATE = 1,
  RL_BITSET = 2,
  RL_BOTH = 3
} RLMode;

typedef enum {
  SHARE_BELOW = 0,
  SHARE_ABOVE = 1
} SharingClass;

typedef struct {
  bool valid;
  unsigned int tag;
  MESIState state_at_eviction;
  SharingClass sharing_at_eviction;
  unsigned long insertion_sequence;
} EvictionHistoryEntry;

typedef struct {
  unsigned long history_hits;
  unsigned long returned_evictions;
  unsigned long positive_rewards;
  unsigned long negative_rewards;
  unsigned long first_candidates_accepted;
  unsigned long second_chances_given;
} RLStats;

typedef struct {
  bool valid;           /* Valid */
  BitArray * tag;       /* Tag */
  unsigned char * data; /* Data */
  unsigned long last_used;
  unsigned long insertion_time;
  unsigned long access_count;
  MESIState mesi_state;
  unsigned int sharer_count;
} CacheLine;

typedef struct {
  CacheLine * lines;    /* n-way: there are n lines */
  EvictionHistoryEntry * history;
  unsigned int history_capacity;
  double q_modified;
  double q_exclusive;
  double q_shared;
  double q_above_threshold;
  double q_below_threshold;
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
  RLMode rl_mode;
  double rl_alpha;
  unsigned int num_cores;
  unsigned long history_sequence;

  CacheSet * sets;    /* sets */
  CacheStats * stats; /* statistics */
  RLStats rl_stats;
} Cache;

Cache* initCache(unsigned int addr_width, unsigned int line_size, unsigned int mem_size, unsigned int cache_size, unsigned int assoc, unsigned int policy);
Cache* initCacheWithRL(unsigned int addr_width, unsigned int line_size, unsigned int mem_size, unsigned int cache_size, unsigned int assoc, unsigned int policy, RLMode rl_mode, double rl_alpha, unsigned int num_cores);
void freeCache(Cache* cache);
void print_cache_config(Cache * cache);
void print_stats(CacheStats * stat);
void print_rl_stats(Cache * cache);

SharingClass classify_sharing(unsigned int sharer_count, unsigned int num_cores);
int history_find_by_tag(CacheSet *set, unsigned int tag);
void history_remove_by_tag(CacheSet *set, unsigned int tag);
int history_find_free(CacheSet *set);
int history_find_oldest(CacheSet *set);
void history_add_eviction_record(Cache *cache, CacheSet *set, unsigned int tag, MESIState state, SharingClass sharing);
void rl_apply_reward(Cache *cache, CacheSet *set, MESIState state, SharingClass sharing, double reward);
