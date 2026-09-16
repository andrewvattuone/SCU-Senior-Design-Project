/*************************************************************
 * cache simulator main body (programming assignment #2)
 *************************************************************/
#include "cache.h"
#include <string.h>
#include <ctype.h>
#define MAX_LINE_LENGTH 200

typedef struct {
    char command;
    unsigned int address;
    unsigned int value;
    bool is_write;
    bool has_metadata;
    MESIState mesi_state;
    unsigned int sharer_count;
} TraceEntry;

#define MAX_TRACE 100000

TraceEntry trace_entries[MAX_TRACE];
unsigned int trace_count = 0;
unsigned int current_trace_index = 0;

int is_comment_or_empty(const char *line) {
    return line[0] == '#' || line[0] == '\0' || isspace(line[0]);
}

unsigned int parse_number(const char *str) {
    if (strstr(str, "0x") == str) {
        return (unsigned int)strtoul(str, NULL, 16); // hexadecimal
    } else {
        return (unsigned int)atoi(str); // decimal
    }
}

int parse_unsigned_strict(const char *str, unsigned int *value) {
    char *end = NULL;
    unsigned long parsed = strtoul(str, &end, 0);
    while (end != NULL && isspace((unsigned char)*end)) {
        end++;
    }
    if (end == str || (end != NULL && *end != '\0') || parsed > 0xFFFFFFFFUL) {
        return 0;
    }
    *value = (unsigned int)parsed;
    return 1;
}

int parse_double_strict(const char *str, double *value) {
    char *end = NULL;
    double parsed = strtod(str, &end);
    while (end != NULL && isspace((unsigned char)*end)) {
        end++;
    }
    if (end == str || (end != NULL && *end != '\0')) {
        return 0;
    }
    *value = parsed;
    return 1;
}

int parse_mesi_token(const char *token, MESIState *state) {
    if (strcmp(token, "M") == 0) {
        *state = MESI_MODIFIED;
        return 1;
    }
    if (strcmp(token, "E") == 0) {
        *state = MESI_EXCLUSIVE;
        return 1;
    }
    if (strcmp(token, "S") == 0) {
        *state = MESI_SHARED;
        return 1;
    }
    return 0;
}

unsigned int getTagValue(BitArray *tag) {
    unsigned int value = 0;

    for (size_t i = 0; i < tag->size; i++) {
        if (getBit(tag, i))
            value |= (1U << i);
    }

    return value;
}

void setTagValue(BitArray *tag, unsigned int value) {
    for (size_t i = 0; i < tag->size; i++) {
        if ((value >> i) & 1)
            setBit(tag, i);
        else
            clearBit(tag, i);
    }
}

unsigned int set_index_mask(Cache *cache) {
    if (cache->set_index_bits == 0) {
        return 0;
    }
    if (cache->set_index_bits >= (sizeof(unsigned int) * 8)) {
        return 0xFFFFFFFFU;
    }
    return (1U << cache->set_index_bits) - 1U;
}

int selectBeladyVictim(Cache *cache, CacheSet *set) {
    int victim = 0;
    unsigned int farthest_next_use = 0;

    for (int way = 0; way < cache->num_ways; way++) {
        unsigned int cached_tag = getTagValue(set->lines[way].tag);

        unsigned int next_use = 0xFFFFFFFF;

        for (unsigned int i = current_trace_index + 1; i < trace_count; i++) {
            unsigned int future_address = trace_entries[i].address;

            unsigned int future_set_index =
                (future_address >> cache->line_size) &
                set_index_mask(cache);

            unsigned int future_tag =
                future_address >> (cache->line_size + cache->set_index_bits);

            if (&cache->sets[future_set_index] == set &&
                future_tag == cached_tag) {
                next_use = i;
                break;
            }
        }

        if (next_use == 0xFFFFFFFF) {
            return way;
        }

        if (next_use > farthest_next_use) {
            farthest_next_use = next_use;
            victim = way;
        }
    }

    return victim;
}

int selectBaselineVictim(Cache *cache, CacheSet *set, int excluded_way) {

    // Otherwise use replacement policy
    switch (cache->policy) {

        case 0: { // Random
            int victim = rand() % cache->num_ways;
            while (victim == excluded_way) {
                victim = rand() % cache->num_ways;
            }
            return victim;
        }
        case 1: {// LRU
            int victim = -1;

            for (int i = 0; i < cache->num_ways; i++) {
                if (i == excluded_way)
                    continue;

                if (victim < 0 ||
                    set->lines[i].last_used <
                    set->lines[victim].last_used)
                {
                    victim = i;
                }
            }

            return victim;
        }

        case 2: {// FIFO
            int victim = -1;

            for (int i = 0; i < cache->num_ways; i++) {
                if (i == excluded_way)
                    continue;
                if (victim < 0 ||
                    set->lines[i].insertion_time <
                    set->lines[victim].insertion_time) {
                    victim = i;
                }
            }

            return victim;
        }

        case 3: { // LFU
            int victim = -1;

            for (int i = 0; i < cache->num_ways; i++) {
                if (i == excluded_way)
                    continue;

                if (victim < 0 ||
                    set->lines[i].access_count <
                    set->lines[victim].access_count)
                {
                    victim = i;
                }
            }

            return victim;
        }

        case 5: { // Belady Optimal
            return selectBeladyVictim(cache, set);
        }
    }
    return 0;
}

double rl_candidate_score(Cache *cache, CacheSet *set, CacheLine *line) {
    double state_q = 0.0;
    double sharing_q = 0.0;
    SharingClass sharing = classify_sharing(line->sharer_count, cache->num_cores);

    if (line->mesi_state == MESI_MODIFIED) {
        state_q = set->q_modified;
    } else if (line->mesi_state == MESI_EXCLUSIVE) {
        state_q = set->q_exclusive;
    } else if (line->mesi_state == MESI_SHARED) {
        state_q = set->q_shared;
    }

    sharing_q = (sharing == SHARE_ABOVE) ? set->q_above_threshold : set->q_below_threshold;

    if (cache->rl_mode == RL_STATE) {
        return state_q;
    }
    if (cache->rl_mode == RL_BITSET) {
        return sharing_q;
    }
    return (state_q + sharing_q) / 2.0;
}

int selectVictim(Cache *cache, CacheSet *set) {

    // First look for an empty line
    for (int i = 0; i < cache->num_ways; i++) {
        if (!set->lines[i].valid)
            return i;
    }

    int first_victim = selectBaselineVictim(cache, set, -1);

    if ((cache->policy != 1 && cache->policy != 3) || cache->rl_mode == RL_DISABLED) {
        return first_victim;
    }

    cache->rl_stats.rl_decisions++;

    /* Epsilon-greedy: with probability rl_epsilon, ignore the learned score and
       pick ACCEPT/PROTECT at random. Never explores when rl_epsilon is 0.0
       (no rand() calls, so legacy behavior is bit-for-bit identical) and never
       when there is no alternate victim to protect with (num_ways < 2). */
    if (cache->rl_epsilon > 0.0 && cache->num_ways >= 2 &&
        ((double)rand() / ((double)RAND_MAX + 1.0)) < cache->rl_epsilon) {
        cache->rl_stats.exploration_decisions++;
        if (rand() % 2 == 0) {
            cache->rl_stats.exploration_accepts++;
            cache->rl_stats.first_candidates_accepted++;
            return first_victim;
        }
        cache->rl_stats.exploration_protects++;
        cache->rl_stats.second_chances_given++;
        return selectBaselineVictim(cache, set, first_victim);
    }

    cache->rl_stats.exploitation_decisions++;

    double candidate_score = rl_candidate_score(cache, set, &set->lines[first_victim]);
    if (candidate_score >= 0.0 || cache->num_ways < 2) {
        cache->rl_stats.first_candidates_accepted++;
        return first_victim;
    }

    cache->rl_stats.second_chances_given++;
    return selectBaselineVictim(cache, set, first_victim);
}

void accessCacheWithMetadata(Cache *cache, unsigned int address, bool is_write, unsigned int value, MESIState mesi_state, unsigned int sharer_count, bool has_metadata) {
    cache->stats->num_of_total_accesses++;

    unsigned int set_index = (address >> cache->line_size) & set_index_mask(cache);

    unsigned int tag = address >> (cache->line_size + cache->set_index_bits);

    unsigned int offset = address & ((1 << cache->line_size) - 1);

    CacheSet *set = &cache->sets[set_index];

    int hit = 0;

    for (int i = 0; i < cache->num_ways; i++) {

        if (!set->lines[i].valid)
            continue;

        if (getTagValue(set->lines[i].tag) == tag) {

            hit = 1;
            cache->stats->num_of_hits++;
            set->lines[i].last_used = cache->stats->num_of_total_accesses;
            set->lines[i].access_count++;
            if (has_metadata) {
                set->lines[i].mesi_state = mesi_state;
                set->lines[i].sharer_count = sharer_count;
            }

            if (is_write) {
                set->lines[i].data[offset] = (unsigned char)value;
            }
            break;
        }
    }

    if (!hit) {
        cache->stats->num_of_total_misses++;

        if (cache->rl_mode != RL_DISABLED) {
            int returned = history_find_by_tag(set, tag);
            if (returned >= 0) {
                rl_apply_reward(cache,
                    set,
                    set->history[returned].state_at_eviction,
                    set->history[returned].sharing_at_eviction,
                    -1.0);
                cache->rl_stats.history_hits++;
                cache->rl_stats.returned_evictions++;
                cache->rl_stats.negative_rewards++;
                history_remove_by_tag(set, tag);
            }
        }

        int victim = selectVictim(cache, set);
        bool evicting_valid_line = set->lines[victim].valid;
        unsigned int evicted_tag = getTagValue(set->lines[victim].tag);
        MESIState evicted_state = set->lines[victim].mesi_state;
        SharingClass evicted_sharing = classify_sharing(set->lines[victim].sharer_count, cache->num_cores);

        if (cache->rl_mode != RL_DISABLED && evicting_valid_line) {
            history_add_eviction_record(cache, set, evicted_tag, evicted_state, evicted_sharing);
        }

        set->lines[victim].valid = true;
        setTagValue(set->lines[victim].tag, tag);

        set->lines[victim].last_used = cache->stats->num_of_total_accesses;
        set->lines[victim].insertion_time = cache->stats->num_of_total_accesses;
        set->lines[victim].access_count = 1;
        set->lines[victim].mesi_state = has_metadata ? mesi_state : MESI_MODIFIED;
        set->lines[victim].sharer_count = has_metadata ? sharer_count : 0;

        if (is_write) {
            set->lines[victim].data[offset] = (unsigned char)value;
        }
    }
}

void accessCache(Cache *cache, unsigned int address, bool is_write, unsigned int value) {
    accessCacheWithMetadata(cache, address, is_write, value, MESI_MODIFIED, 0, false);
}

Cache * parse_config(FILE *file) {
    char line[MAX_LINE_LENGTH];
    unsigned int addr_width, line_size, mem_size, cache_size, assoc, policy;
    unsigned int rl_mode = 0;
    double rl_alpha = 0.1;
    double rl_epsilon = 0.0;
    unsigned int num_cores = 16;
    Cache * cache = NULL;
    char optional_lines[4][MAX_LINE_LENGTH];
    unsigned int extra_non_comment_lines = 0;

    for (int i = 0; i < 6; i++) {
        if (fgets(line, sizeof(line), file) == NULL) {
            fprintf(stderr, "Error: Unexpected end of file while reading config\n");
            return cache;
        }

        // for debugging: printf("line %d: txt - %s\n",i,line);
        // ignoring comments or empty line
        if (is_comment_or_empty(line)) {
            i--;
            continue;
        }
        switch (i) {
            case 0: addr_width = parse_number(line); break;
            case 1: line_size = parse_number(line); break;
            case 2: mem_size = parse_number(line); break;
            case 3: cache_size = parse_number(line); break;
            case 4: assoc = parse_number(line); break;
            case 5: policy = parse_number(line); break;
        }
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (is_comment_or_empty(line)) {
            continue;
        }
        if (extra_non_comment_lines < 4) {
            strcpy(optional_lines[extra_non_comment_lines], line);
        }
        extra_non_comment_lines++;
    }

    /* Optional RL lines. Backward compatibility is by line count:
       3 lines = legacy format (rl_mode, rl_alpha, num_cores), rl_epsilon defaults to 0.0
       4 lines = new format    (rl_mode, rl_alpha, rl_epsilon, num_cores) */
    if (extra_non_comment_lines > 4) {
        fprintf(stderr, "Error: Too many configuration lines; expected at most 4 optional RL lines (rl_mode, rl_alpha, rl_epsilon, num_cores)\n");
        return NULL;
    }

    if (extra_non_comment_lines > 0) {
        if (!parse_unsigned_strict(optional_lines[0], &rl_mode) || rl_mode > 3) {
            fprintf(stderr, "Error: Invalid rl_mode in configuration\n");
            return NULL;
        }
        if (extra_non_comment_lines >= 2) {
            if (!parse_double_strict(optional_lines[1], &rl_alpha) || rl_alpha <= 0.0 || rl_alpha > 1.0) {
                fprintf(stderr, "Error: Invalid rl_alpha in configuration; expected value in (0, 1]\n");
                return NULL;
            }
        }
        if (extra_non_comment_lines == 3) {
            if (!parse_unsigned_strict(optional_lines[2], &num_cores) || num_cores == 0) {
                fprintf(stderr, "Error: Invalid num_cores in configuration; expected positive integer\n");
                return NULL;
            }
        }
        if (extra_non_comment_lines == 4) {
            if (!parse_double_strict(optional_lines[2], &rl_epsilon) || rl_epsilon < 0.0 || rl_epsilon > 1.0) {
                fprintf(stderr, "Error: Invalid rl_epsilon in configuration; expected value in [0, 1]\n");
                return NULL;
            }
            if (!parse_unsigned_strict(optional_lines[3], &num_cores) || num_cores == 0) {
                fprintf(stderr, "Error: Invalid num_cores in configuration; expected positive integer\n");
                return NULL;
            }
        }
    }

    if (rl_mode != 0 && policy != 1 && policy != 3) {
        fprintf(stderr, "Error: rl_mode is supported only with LRU or LFU replacement policies\n");
        return NULL;
    }

    cache = initCacheWithRL(addr_width,line_size,mem_size,cache_size, assoc,policy,(RLMode)rl_mode,rl_alpha,rl_epsilon,num_cores);
    return cache;
}



// int parse_trace(FILE *file, Cache *cache) {
//     char line[MAX_LINE_LENGTH];
//     unsigned int num=0;
//     while (fgets(line, sizeof(line), file) != NULL) {

//         printf("trace #%u: %s",++num,line);

//         // ignoring comments or empty line
//         if (is_comment_or_empty(line)) {
//             continue;
//         }

//         char command;
//         unsigned int address, value;

//         // parsing command
//         if (sscanf(line, "%c", &command) != 1) {
//             fprintf(stderr, "Error: Invalid trace command\n");
//             return 0;
//         }

//         // Read 
//         if (command == 'R') {
//             if (sscanf(line + 1, "%x", &address) != 1) {
//                 fprintf(stderr, "Error: Invalid address format in READ command\n");
//                 return 0;
//             }
//             printf("READ: Address = 0x%x\n", address);

//             /////////////////////////////////////////////////////
//             // PART 1-4
//             // Implement read here
//             /////////////////////////////////////////////////////
//             accessCache(cache, address, false, 0);
//         }
//         // Write
//         else if (command == 'W') {
//             if (sscanf(line + 1, "%x %x", &address, &value) != 2) {
//                 fprintf(stderr, "Error: Invalid format in WRITE command\n");
//                 return 0;
//             }
//             printf("WRITE: Address = 0x%x, Value = 0x%x\n", address, value);

//             /////////////////////////////////////////////////////
//             // PART 1-4
//             // Implement write here
//             /////////////////////////////////////////////////////

//            accessCache(cache, address, true, value);

//         } else {
//             fprintf(stderr, "Error: Unknown command '%c'\n", command);
//             return 0;
//         }
//     }

//     // Set hit rate
//     if (cache->stats->num_of_total_accesses > 0) {
//         cache->stats->hit_rate = (long double)cache->stats->num_of_hits / cache->stats->num_of_total_accesses;
//     }

//     return 1;
// }

int parse_trace(FILE *file, Cache *cache) {
    char line[MAX_LINE_LENGTH];
    unsigned int num = 0;

    trace_count = 0;
    current_trace_index = 0;

    while (fgets(line, sizeof(line), file) != NULL) {

        printf("trace #%u: %s", ++num, line);

        if (is_comment_or_empty(line)) {
            continue;
        }

        char command;
        unsigned int address, value = 0;
        char mesi_token[8];
        unsigned int sharer_count = 0;
        MESIState mesi_state = MESI_INVALID;
        int metadata_fields = 0;

        if (sscanf(line, "%c", &command) != 1) {
            fprintf(stderr, "Error: Invalid trace command\n");
            return 0;
        }

        if (command == 'R') {
            metadata_fields = sscanf(line + 1, "%x %7s %u", &address, mesi_token, &sharer_count);
            if (metadata_fields < 1) {
                fprintf(stderr, "Error: Invalid address format in READ command\n");
                return 0;
            }
            if (metadata_fields == 3) {
                if (!parse_mesi_token(mesi_token, &mesi_state) && cache->rl_mode != RL_DISABLED) {
                    fprintf(stderr, "Error: Invalid MESI metadata on trace line %u\n", num);
                    return 0;
                }
            } else if (cache->rl_mode != RL_DISABLED) {
                fprintf(stderr, "Error: Missing MESI/sharer metadata on trace line %u while RL is enabled\n", num);
                return 0;
            }

            if (cache->rl_mode != RL_DISABLED && sharer_count > cache->num_cores) {
                fprintf(stderr, "Error: Invalid sharer_count on trace line %u; expected 0..%u\n", num, cache->num_cores);
                return 0;
            }

            printf("READ: Address = 0x%x\n", address);

            if (trace_count >= MAX_TRACE) {
                fprintf(stderr, "Error: Trace contains more than %u usable entries\n", MAX_TRACE);
                return 0;
            }
            trace_entries[trace_count].command = command;
            trace_entries[trace_count].address = address;
            trace_entries[trace_count].value = 0;
            trace_entries[trace_count].is_write = false;
            trace_entries[trace_count].has_metadata = (metadata_fields == 3 && mesi_state != MESI_INVALID);
            trace_entries[trace_count].mesi_state = mesi_state;
            trace_entries[trace_count].sharer_count = sharer_count;
            trace_count++;
        }
        else if (command == 'W') {
            metadata_fields = sscanf(line + 1, "%x %x %7s %u", &address, &value, mesi_token, &sharer_count);
            if (metadata_fields < 2) {
                fprintf(stderr, "Error: Invalid format in WRITE command\n");
                return 0;
            }
            if (metadata_fields == 4) {
                if (!parse_mesi_token(mesi_token, &mesi_state) && cache->rl_mode != RL_DISABLED) {
                    fprintf(stderr, "Error: Invalid MESI metadata on trace line %u\n", num);
                    return 0;
                }
            } else if (cache->rl_mode != RL_DISABLED) {
                fprintf(stderr, "Error: Missing MESI/sharer metadata on trace line %u while RL is enabled\n", num);
                return 0;
            }

            if (cache->rl_mode != RL_DISABLED && sharer_count > cache->num_cores) {
                fprintf(stderr, "Error: Invalid sharer_count on trace line %u; expected 0..%u\n", num, cache->num_cores);
                return 0;
            }

            printf("WRITE: Address = 0x%x, Value = 0x%x\n", address, value);

            if (trace_count >= MAX_TRACE) {
                fprintf(stderr, "Error: Trace contains more than %u usable entries\n", MAX_TRACE);
                return 0;
            }
            trace_entries[trace_count].command = command;
            trace_entries[trace_count].address = address;
            trace_entries[trace_count].value = value;
            trace_entries[trace_count].is_write = true;
            trace_entries[trace_count].has_metadata = (metadata_fields == 4 && mesi_state != MESI_INVALID);
            trace_entries[trace_count].mesi_state = mesi_state;
            trace_entries[trace_count].sharer_count = sharer_count;
            trace_count++;
        }
        else {
            fprintf(stderr, "Error: Unknown command '%c'\n", command);
            return 0;
        }

    }

    for (current_trace_index = 0;
         current_trace_index < trace_count;
         current_trace_index++) {

        accessCacheWithMetadata(cache,
            trace_entries[current_trace_index].address,
            trace_entries[current_trace_index].is_write,
            trace_entries[current_trace_index].value,
            trace_entries[current_trace_index].mesi_state,
            trace_entries[current_trace_index].sharer_count,
            trace_entries[current_trace_index].has_metadata);
    }

    if (cache->stats->num_of_total_accesses > 0) {
        cache->stats->hit_rate = (long double)cache->stats->num_of_hits / cache->stats->num_of_total_accesses;
    }

    return 1;
}


int main(int argc, char *argv[]) {

    if (argc != 3) {
        printf("Usage: cache_sim <config_file> <trace_file>\n");
        return 1;
    }

    // open configuration file
    FILE *config_file = fopen(argv[1], "r");
    if (config_file == NULL) {
        printf("Error: Could not open file '%s'\n", argv[1]);
        printf("Usage: cache_sim <config_file> <trace_file>\n");
        return 1;
    }
    printf("Successfully opened the configuration file: %s\n", argv[1]);

    // open trace file
    FILE *trace_file = fopen(argv[2], "r");
    if (trace_file == NULL) {
        printf("Error: Could not open file '%s'\n", argv[2]);
        printf("Usage: cache_sim <config_file> <trace_file>\n");
        return 1;
    }
    printf("Successfully opened the trace file: %s\n", argv[2]);


    Cache *  my_cache = parse_config(config_file);
    if(my_cache==NULL) {
           printf("Configuration was not complte\n");
           return 1;
    }
    // close the config file
    fclose(config_file);

    // print out the configuration
    print_cache_config(my_cache);
    // parse trace
    if (!parse_trace(trace_file, my_cache)) {
        fclose(trace_file);
        freeCache(my_cache);
        return 1;
    }
    // close the trace file
    fclose(trace_file);
    // print out the statistics
    print_stats(my_cache->stats);
    print_rl_stats(my_cache);
    // freeing cache
    freeCache(my_cache);    

    
    return 0;
}
