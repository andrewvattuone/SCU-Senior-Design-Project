/*******************************************************
 * cache data structure for programming assignment #2
 *******************************************************/
#include "cache.h"

static void freePartialCache(Cache *cache, unsigned int initialized_sets) {
    if (cache == NULL) {
        return;
    }

    if (cache->sets != NULL) {
        for (unsigned int i = 0; i < initialized_sets; i++) {
            if (cache->sets[i].lines != NULL) {
                for (unsigned int j = 0; j < cache->num_ways; j++) {
                    if (cache->sets[i].lines[j].tag != NULL) {
                        freeBitArray(cache->sets[i].lines[j].tag);
                    }
                    free(cache->sets[i].lines[j].data);
                }
                free(cache->sets[i].lines);
            }
            free(cache->sets[i].history);
        }
        free(cache->sets);
    }
    free(cache->stats);
    free(cache);
}

/* initalize the cache memory with the given set of parameters */
Cache* initCacheWithRL(unsigned int addr_width, unsigned int line_size, unsigned int mem_size, unsigned int cache_size, unsigned int assoc, unsigned int policy, RLMode rl_mode, double rl_alpha, double rl_epsilon, unsigned int num_cores) {

    if (rl_epsilon < 0.0 || rl_epsilon > 1.0) {
        fprintf(stderr, "Error: Invalid rl_epsilon %f; expected value in [0, 1]\n", rl_epsilon);
        return NULL;
    }

    Cache* cache = (Cache*)calloc(1, sizeof(Cache));
    if (cache == NULL) {
        fprintf(stderr, "Error: Failed to allocate cache\n");
        return NULL;
    }

    printf("addr_width %u, line_size %u, mem_size %u, cache_size %u, assoc %u, pol %u\n",
                    addr_width,
                    line_size,
                    mem_size,
                    cache_size,
                    assoc,
                    policy);

    
    cache->addr_width = addr_width;
    cache->line_size = line_size;
    cache->mem_size = mem_size;
    cache->cache_size = cache_size;
    cache->assoc = assoc;
    cache->policy = policy;
    cache->rl_mode = rl_mode;
    cache->rl_alpha = rl_alpha;
    cache->rl_epsilon = rl_epsilon;
    cache->num_cores = num_cores;
    cache->history_sequence = 0;

    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////
    // TO BE COMPLETED FOR PART 0
    // How many sets?
    cache->num_lines = 1 << (cache_size - line_size); // total number of cache lines
    cache->num_sets = (assoc == 0) ? 1 : cache->num_lines / assoc; // total number of sets (fully-associative: 1 set)
    cache->num_ways = (assoc == 0) ? cache->num_lines : assoc;
    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////
    int line_offset_bits = line_size;
    
    // calculate log2 to then get the tag size
    cache->set_index_bits = 0;
    for (unsigned int temp = cache->num_sets; temp > 1; temp >>= 1) {
        cache->set_index_bits++;
    }
    cache->tag_size = addr_width - cache->set_index_bits - line_offset_bits;

    // Memory allocation for sets
    cache->sets = (CacheSet*)calloc(cache->num_sets, sizeof(CacheSet));
    if (cache->sets == NULL) {
        fprintf(stderr, "Error: Failed to allocate cache sets\n");
        freePartialCache(cache, 0);
        return NULL;
    }

    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////
    // TO BE COMPLETED FOR PART 0
    // Set initialization
    for (unsigned int i = 0; i < cache->num_sets; i++) {

        // TO BE
        cache->sets[i].lines = (CacheLine*)calloc(cache->num_ways, sizeof(CacheLine));
        cache->sets[i].history_capacity = 2 * cache->num_ways;
        cache->sets[i].history = (EvictionHistoryEntry*)calloc(cache->sets[i].history_capacity, sizeof(EvictionHistoryEntry));
        if (cache->sets[i].lines == NULL || cache->sets[i].history == NULL) {
            fprintf(stderr, "Error: Failed to allocate cache set %u\n", i);
            freePartialCache(cache, i + 1);
            return NULL;
        }

        // cache line initialization
        for (unsigned int j = 0; j < cache->num_ways; j++) {

            // initialization for Valid
            cache->sets[i].lines[j].valid = false;

            // Initalize Tag/Data 
            cache->sets[i].lines[j].tag = createBitArray(cache->tag_size);
            cache->sets[i].lines[j].data = (unsigned char*)calloc(1, 1 << line_size);
            if (cache->sets[i].lines[j].tag == NULL || cache->sets[i].lines[j].data == NULL) {
                fprintf(stderr, "Error: Failed to allocate cache line %u in set %u\n", j, i);
                freePartialCache(cache, i + 1);
                return NULL;
            }
            cache->sets[i].lines[j].last_used = 0;
            cache->sets[i].lines[j].insertion_time = 0;
            cache->sets[i].lines[j].access_count = 0;
            cache->sets[i].lines[j].mesi_state = MESI_INVALID;
            cache->sets[i].lines[j].sharer_count = 0;
        }
    }
    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////

    // Stats initialization
    cache->stats = (CacheStats*)malloc(sizeof(CacheStats));
    if (cache->stats == NULL) {
        fprintf(stderr, "Error: Failed to allocate cache statistics\n");
        freePartialCache(cache, cache->num_sets);
        return NULL;
    }
    cache->stats->num_of_total_accesses = 0;
    cache->stats->num_of_hits = 0;
    cache->stats->num_of_total_misses = 0;
    cache->stats->num_of_compulsory_misses = 0;
    cache->stats->num_of_capacity_misses = 0;
    cache->stats->num_of_conflict_misses = 0;
    cache->stats->hit_rate = 0.0;

    //print_cache_config(cache);

    return cache;
}

Cache* initCache(unsigned int addr_width, unsigned int line_size, unsigned int mem_size, unsigned int cache_size, unsigned int assoc, unsigned int policy) {
    return initCacheWithRL(addr_width, line_size, mem_size, cache_size, assoc, policy, RL_DISABLED, 0.1, 0.0, 16);
}

void freeCache(Cache* cache) {
    if (cache == NULL) {
        return;
    }
    int num_lines = (1 << (cache->cache_size - cache->line_size));
    int num_sets = cache->assoc == 0 ? 1 : num_lines / cache->assoc;
    int num_ways = cache->assoc == 0 ? num_lines : cache->assoc;

    for (int i = 0; i < num_sets; i++) {
        for (int j = 0; j < num_ways; j++) {
            freeBitArray(cache->sets[i].lines[j].tag);
            free(cache->sets[i].lines[j].data);
        }
        free(cache->sets[i].lines);
        free(cache->sets[i].history);
    }
    free(cache->sets);
    free(cache->stats);
    free(cache);
}

SharingClass classify_sharing(unsigned int sharer_count, unsigned int num_cores) {
    return (sharer_count > (num_cores / 2)) ? SHARE_ABOVE : SHARE_BELOW;
}

int history_find_by_tag(CacheSet *set, unsigned int tag) {
    for (unsigned int i = 0; i < set->history_capacity; i++) {
        if (set->history[i].valid && set->history[i].tag == tag) {
            return (int)i;
        }
    }
    return -1;
}

void history_remove_by_tag(CacheSet *set, unsigned int tag) {
    int index = history_find_by_tag(set, tag);
    if (index >= 0) {
        set->history[index].valid = false;
    }
}

int history_find_free(CacheSet *set) {
    for (unsigned int i = 0; i < set->history_capacity; i++) {
        if (!set->history[i].valid) {
            return (int)i;
        }
    }
    return -1;
}

int history_find_oldest(CacheSet *set) {
    int oldest = -1;
    for (unsigned int i = 0; i < set->history_capacity; i++) {
        if (!set->history[i].valid) {
            continue;
        }
        if (oldest < 0 || set->history[i].insertion_sequence < set->history[oldest].insertion_sequence) {
            oldest = (int)i;
        }
    }
    return oldest;
}

static double clamp_q(double value) {
    if (value > 1.0) {
        return 1.0;
    }
    if (value < -1.0) {
        return -1.0;
    }
    return value;
}

void rl_apply_reward(Cache *cache, CacheSet *set, MESIState state, SharingClass sharing, double reward) {
    if (cache->rl_mode == RL_DISABLED) {
        return;
    }

    if (cache->rl_mode == RL_STATE || cache->rl_mode == RL_BOTH) {
        double *state_q = NULL;
        if (state == MESI_MODIFIED) {
            state_q = &set->q_modified;
        } else if (state == MESI_EXCLUSIVE) {
            state_q = &set->q_exclusive;
        } else if (state == MESI_SHARED) {
            state_q = &set->q_shared;
        }
        if (state_q != NULL) {
            *state_q = clamp_q(*state_q + cache->rl_alpha * (reward - *state_q));
        }
    }

    if (cache->rl_mode == RL_BITSET || cache->rl_mode == RL_BOTH) {
        double *sharing_q = (sharing == SHARE_ABOVE) ? &set->q_above_threshold : &set->q_below_threshold;
        *sharing_q = clamp_q(*sharing_q + cache->rl_alpha * (reward - *sharing_q));
    }
}

void history_add_eviction_record(Cache *cache, CacheSet *set, unsigned int tag, MESIState state, SharingClass sharing) {
    int existing = history_find_by_tag(set, tag);
    if (existing >= 0) {
        set->history[existing].valid = false;
    }

    int slot = history_find_free(set);
    if (slot < 0) {
        slot = history_find_oldest(set);
        if (slot >= 0) {
            rl_apply_reward(cache,
                set,
                set->history[slot].state_at_eviction,
                set->history[slot].sharing_at_eviction,
                1.0);
            cache->rl_stats.positive_rewards++;
        }
    }

    if (slot >= 0) {
        set->history[slot].valid = true;
        set->history[slot].tag = tag;
        set->history[slot].state_at_eviction = state;
        set->history[slot].sharing_at_eviction = sharing;
        set->history[slot].insertion_sequence = ++cache->history_sequence;
    }
}

bool isPowerOfTwo(int x) { return (x !=0) && ((x & (x-1))==0); }

void print_cache_config(Cache * cache){
        printf("================= Cache Configuration =======================\n");
        printf("address width: %u bit \n",cache->addr_width);
        printf("line size: %u Bytes\n",(unsigned int)pow(2,cache->line_size));
        printf("primary memory size: %u Bytes\n",(unsigned int)pow(2,cache->mem_size));
        printf("cache size: %u Bytes\n",(unsigned int)pow(2,cache->cache_size));
        printf("associativity: ");
        if(cache->assoc==0) printf("fully associative\n");
        else if(cache->assoc==1) printf("direct-mapped\n");
        else if(isPowerOfTwo(cache->assoc)) printf("%d-way\n",cache->assoc);
        else printf("error\n");
        printf("replacement policy: ");
        if(cache->policy==0) printf("random\n");
        else if(cache->policy==1) printf("LRU\n");
        else if(cache->policy==2) printf("FIFO\n");
        else if(cache->policy==3) printf("LFU\n");
        else if(cache->policy==5) printf("Belady Optimal\n");
        else printf("error\n");
        if(cache->rl_mode!=0) {
            if(cache->rl_mode==1) printf("RL mode: RL-State\n");
            else if(cache->rl_mode==2) printf("RL mode: RL-Bitset\n");
            else if(cache->rl_mode==3) printf("RL mode: RL-Both\n");
            printf("RL alpha: %.6f\n", cache->rl_alpha);
            printf("RL epsilon: %.6f\n", cache->rl_epsilon);
            printf("num cores: %u\n", cache->num_cores);
        }

        return;
}
void print_stats(CacheStats * stat){
        printf("================= Cache Statistics  =======================\n");
        printf("for %lu accesses ... \n",stat->num_of_total_accesses);
        printf("hits: %lu \n",stat->num_of_hits);
        printf("compulsory misses: %lu \n",stat->num_of_compulsory_misses);
        printf("capacity misses: %lu \n",stat->num_of_capacity_misses);
        printf("conflict misses: %lu \n",stat->num_of_conflict_misses);
        printf("cache hit rate: %Lf\n",stat->hit_rate); 
        return;
}

void print_rl_stats(Cache * cache){
        if(cache->rl_mode==RL_DISABLED) {
            return;
        }

        printf("================= RL Statistics  =======================\n");
        printf("history hits / returned evictions: %lu / %lu\n", cache->rl_stats.history_hits, cache->rl_stats.returned_evictions);
        printf("positive rewards: %lu\n", cache->rl_stats.positive_rewards);
        printf("negative rewards: %lu\n", cache->rl_stats.negative_rewards);
        printf("first candidates accepted: %lu\n", cache->rl_stats.first_candidates_accepted);
        printf("second chances given: %lu\n", cache->rl_stats.second_chances_given);
        printf("RL decisions: %lu\n", cache->rl_stats.rl_decisions);
        printf("exploration decisions: %lu\n", cache->rl_stats.exploration_decisions);
        printf("exploitation decisions: %lu\n", cache->rl_stats.exploitation_decisions);
        printf("exploration accepts: %lu\n", cache->rl_stats.exploration_accepts);
        printf("exploration protects: %lu\n", cache->rl_stats.exploration_protects);

        if(cache->num_sets <= 16) {
            for(unsigned int i = 0; i < cache->num_sets; i++) {
                CacheSet *set = &cache->sets[i];
                printf("set %u Q: M %.6f E %.6f S %.6f above %.6f below %.6f\n",
                    i,
                    set->q_modified,
                    set->q_exclusive,
                    set->q_shared,
                    set->q_above_threshold,
                    set->q_below_threshold);
            }
        } else {
            double q_modified = 0.0;
            double q_exclusive = 0.0;
            double q_shared = 0.0;
            double q_above = 0.0;
            double q_below = 0.0;
            for(unsigned int i = 0; i < cache->num_sets; i++) {
                q_modified += cache->sets[i].q_modified;
                q_exclusive += cache->sets[i].q_exclusive;
                q_shared += cache->sets[i].q_shared;
                q_above += cache->sets[i].q_above_threshold;
                q_below += cache->sets[i].q_below_threshold;
            }
            printf("average Q: M %.6f E %.6f S %.6f above %.6f below %.6f\n",
                q_modified / cache->num_sets,
                q_exclusive / cache->num_sets,
                q_shared / cache->num_sets,
                q_above / cache->num_sets,
                q_below / cache->num_sets);
        }
        return;
}
