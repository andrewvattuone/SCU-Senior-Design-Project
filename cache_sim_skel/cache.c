/*******************************************************
 * cache data structure for programming assignment #2
 *******************************************************/
#include "cache.h"

/* initalize the cache memory with the given set of parameters */
Cache* initCache(unsigned int addr_width, unsigned int line_size, unsigned int mem_size, unsigned int cache_size, unsigned int assoc, unsigned int policy) {

    Cache* cache = (Cache*)malloc(sizeof(Cache));

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

    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////
    // TO BE COMPLETED FOR PART 0
    // How many sets?
    cache->num_lines = 1 << (cache_size - line_size); // total number of cache lines
    cache->num_sets = (assoc == 0) ? 1 : cache->num_lines / assoc; // total number of sets (fully-associative: 1 set)
    cache->num_ways = (assoc == 0) ? cache->num_lines : assoc;
    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////

    // Memory allocation for sets
    cache->sets = (CacheSet*)malloc(cache->num_sets * sizeof(CacheSet));

    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////
    // TO BE COMPLETED FOR PART 0
    // Set initialization
    for (int i = 0; i < cache->num_sets; i++) {

        // TO BE
        cache->sets[i].lines = (CacheLine*)malloc(cache->num_ways * sizeof(CacheLine));

        // cache line initialization
        for (int j = 0; j < cache->num_ways; j++) {

            // initialization for Valid
            cache->sets[i].lines[j].valid = false;

            // calculate the number of bits in a Tag (addr_width - set_index_bits - line_offset_bits)
            int line_offset_bits = line_size;
            
            // computes log2(num_sets)
            cache->set_index_bits = 0; 
            for (int temp = cache->num_sets; temp > 1; temp >>= 1) 
                cache->set_index_bits++;

            cache->tag_size = addr_width - cache->set_index_bits - line_offset_bits;

            // Initalize Tag/Data 
            cache->sets[i].lines[j].tag = createBitArray(cache->tag_size);
            cache->sets[i].lines[j].data = (unsigned char*)malloc(1 << line_size);
            cache->sets[i].lines[j].last_used = 0;
            cache->sets[i].lines[j].insertion_time = 0;
            cache->sets[i].lines[j].access_count = 0;
        }
    }
    ////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////

    // Stats initialization
    cache->stats = (CacheStats*)malloc(sizeof(CacheStats));
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

void freeCache(Cache* cache) {
    int num_lines = (1 << (cache->cache_size - cache->line_size));
    int num_sets = cache->assoc == 0 ? 1 : num_lines / cache->assoc;
    int num_ways = cache->assoc == 0 ? num_lines : cache->assoc;

    for (int i = 0; i < num_sets; i++) {
        for (int j = 0; j < num_ways; j++) {
            freeBitArray(cache->sets[i].lines[j].tag);
            free(cache->sets[i].lines[j].data);
        }
        free(cache->sets[i].lines);
    }
    free(cache->sets);
    free(cache->stats);
    free(cache);
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
