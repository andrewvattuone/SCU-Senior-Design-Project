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

unsigned int getTagValue(BitArray *tag) {
    unsigned int value = 0;

    for (size_t i = 0; i < tag->size; i++) {
        if (getBit(tag, i))
            value |= (1 << i);
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
                ((1 << cache->set_index_bits) - 1);

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

int selectVictim(Cache *cache, CacheSet *set) {
    
    // First look for an empty line
    for (int i = 0; i < cache->num_ways; i++) {
        if (!set->lines[i].valid)
            return i;
    }

    // Otherwise use replacement policy
    switch (cache->policy) {

        case 0: { // Random
            return rand() % cache->num_ways;
        }
        case 1: {// LRU
            int victim = 0;

            for (int i = 1; i < cache->num_ways; i++) {

                if (set->lines[i].last_used <
                    set->lines[victim].last_used)
                {
                    victim = i;
                }
            }

            return victim;
        }

        case 2: {// FIFO
            int victim = 0;

            for (int i = 1; i < cache->num_ways; i++) {
                if (set->lines[i].insertion_time <
                    set->lines[victim].insertion_time) {
                    victim = i;
                }
            }

            return victim;
        }

        case 3: { // LFU
            int victim = 0;

            for (int i = 1; i < cache->num_ways; i++) {

                if (set->lines[i].access_count <
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

void accessCache(Cache *cache, unsigned int address, bool is_write, unsigned int value) {
    cache->stats->num_of_total_accesses++;

    unsigned int set_index = (address >> cache->line_size) & ((1 << cache->set_index_bits) - 1);

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

            if (is_write) {
                set->lines[i].data[offset] = (unsigned char)value;
            }
            break;
        }
    }

    if (!hit) {
        cache->stats->num_of_total_misses++;

        int victim = selectVictim(cache, set);

        set->lines[victim].valid = true;
        setTagValue(set->lines[victim].tag, tag);

        set->lines[victim].last_used = cache->stats->num_of_total_accesses;
        set->lines[victim].insertion_time = cache->stats->num_of_total_accesses;
        set->lines[victim].access_count = 1;

        if (is_write) {
            set->lines[victim].data[offset] = (unsigned char)value;
        }
    }
}

Cache * parse_config(FILE *file) {
    char line[MAX_LINE_LENGTH];
    unsigned int addr_width, line_size, mem_size, cache_size, assoc, policy;
    Cache * cache = NULL;

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
    cache = initCache(addr_width,line_size,mem_size,cache_size, assoc,policy);
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

        if (sscanf(line, "%c", &command) != 1) {
            fprintf(stderr, "Error: Invalid trace command\n");
            return 0;
        }

        if (command == 'R') {
            if (sscanf(line + 1, "%x", &address) != 1) {
                fprintf(stderr, "Error: Invalid address format in READ command\n");
                return 0;
            }

            printf("READ: Address = 0x%x\n", address);

            trace_entries[trace_count].command = command;
            trace_entries[trace_count].address = address;
            trace_entries[trace_count].value = 0;
            trace_entries[trace_count].is_write = false;
            trace_count++;
        }
        else if (command == 'W') {
            if (sscanf(line + 1, "%x %x", &address, &value) != 2) {
                fprintf(stderr, "Error: Invalid format in WRITE command\n");
                return 0;
            }

            printf("WRITE: Address = 0x%x, Value = 0x%x\n", address, value);

            trace_entries[trace_count].command = command;
            trace_entries[trace_count].address = address;
            trace_entries[trace_count].value = value;
            trace_entries[trace_count].is_write = true;
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

        accessCache(cache,
            trace_entries[current_trace_index].address,
            trace_entries[current_trace_index].is_write,
            trace_entries[current_trace_index].value);
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
    parse_trace(trace_file, my_cache);
    // close the trace file
    fclose(trace_file);
    // print out the statistics
    print_stats(my_cache->stats);
    // freeing cache
    freeCache(my_cache);    

    
    return 0;
}
