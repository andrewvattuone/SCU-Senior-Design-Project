#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define main cache_sim_main
#include "../cache_sim_skel/main.c"
#undef main

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while (0)

static int nearly_equal(double a, double b) {
    return fabs(a - b) < 0.000001;
}

static unsigned int history_count_for_tag(CacheSet *set, unsigned int tag) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < set->history_capacity; i++) {
        if (set->history[i].valid && set->history[i].tag == tag) {
            count++;
        }
    }
    return count;
}

static int test_history_capacity_and_invalid_fill(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 0.0, 4);
    ASSERT_TRUE(cache != NULL, "cache allocation");
    ASSERT_TRUE(cache->sets[0].history_capacity == 4, "history capacity is 2 * associativity");

    accessCacheWithMetadata(cache, 0x0, false, 0, MESI_MODIFIED, 1, true);
    accessCacheWithMetadata(cache, 0x4, false, 0, MESI_EXCLUSIVE, 1, true);
    ASSERT_TRUE(cache->rl_stats.first_candidates_accepted == 0, "invalid fills do not run RL decision");
    ASSERT_TRUE(cache->rl_stats.positive_rewards == 0, "invalid fills do not create positive rewards");
    ASSERT_TRUE(cache->rl_stats.negative_rewards == 0, "invalid fills do not create negative rewards");
    ASSERT_TRUE(cache->rl_stats.rl_decisions == 0, "invalid fills do not count as RL decisions");
    ASSERT_TRUE(cache->rl_stats.exploration_decisions == 0, "invalid fills do not count as exploration");
    ASSERT_TRUE(cache->rl_stats.exploitation_decisions == 0, "invalid fills do not count as exploitation");

    freeCache(cache);
    return 0;
}

static int test_history_helpers_and_positive_reward(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 0.0, 4);
    CacheSet *set = &cache->sets[0];
    ASSERT_TRUE(cache != NULL, "cache allocation");

    history_add_eviction_record(cache, set, 7, MESI_MODIFIED, SHARE_BELOW);
    history_add_eviction_record(cache, set, 7, MESI_EXCLUSIVE, SHARE_ABOVE);
    ASSERT_TRUE(history_count_for_tag(set, 7) == 1, "history keeps one record per tag");
    ASSERT_TRUE(history_find_by_tag(set, 7) >= 0, "history find by tag");
    history_remove_by_tag(set, 7);
    ASSERT_TRUE(history_find_by_tag(set, 7) < 0, "history remove by tag");

    history_add_eviction_record(cache, set, 10, MESI_MODIFIED, SHARE_BELOW);
    history_add_eviction_record(cache, set, 11, MESI_EXCLUSIVE, SHARE_BELOW);
    history_add_eviction_record(cache, set, 12, MESI_SHARED, SHARE_ABOVE);
    history_add_eviction_record(cache, set, 13, MESI_SHARED, SHARE_BELOW);
    history_add_eviction_record(cache, set, 14, MESI_EXCLUSIVE, SHARE_ABOVE);
    ASSERT_TRUE(cache->rl_stats.positive_rewards == 1, "full history expires oldest once");
    ASSERT_TRUE(history_find_by_tag(set, 10) < 0, "oldest history record was replaced");
    ASSERT_TRUE(history_find_by_tag(set, 14) >= 0, "new history record inserted");
    ASSERT_TRUE(nearly_equal(set->q_modified, 0.5), "positive reward updates Q with alpha");

    freeCache(cache);
    return 0;
}

static int test_negative_reward_once_and_q_update(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 0.0, 4);
    CacheSet *set = &cache->sets[0];
    ASSERT_TRUE(cache != NULL, "cache allocation");

    history_add_eviction_record(cache, set, 0, MESI_MODIFIED, SHARE_BELOW);
    accessCacheWithMetadata(cache, 0x0, false, 0, MESI_MODIFIED, 1, true);
    ASSERT_TRUE(cache->rl_stats.history_hits == 1, "returned eviction counted");
    ASSERT_TRUE(cache->rl_stats.returned_evictions == 1, "returned eviction alias counted");
    ASSERT_TRUE(cache->rl_stats.negative_rewards == 1, "negative reward counted once");
    ASSERT_TRUE(history_find_by_tag(set, 0) < 0, "returned history record removed");
    ASSERT_TRUE(nearly_equal(set->q_modified, -0.5), "negative reward updates Q");

    accessCacheWithMetadata(cache, 0x0, false, 0, MESI_MODIFIED, 1, true);
    ASSERT_TRUE(cache->rl_stats.negative_rewards == 1, "removed history entry is not rewarded twice");

    freeCache(cache);
    return 0;
}

static int test_rl_modes_update_scope(void) {
    Cache *state = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 0.0, 4);
    Cache *bitset = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_BITSET, 0.5, 0.0, 4);
    Cache *both = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_BOTH, 0.5, 0.0, 4);
    ASSERT_TRUE(state != NULL && bitset != NULL && both != NULL, "cache allocation");

    rl_apply_reward(state, &state->sets[0], MESI_MODIFIED, SHARE_ABOVE, 1.0);
    ASSERT_TRUE(nearly_equal(state->sets[0].q_modified, 0.5), "RL-State updates state Q");
    ASSERT_TRUE(nearly_equal(state->sets[0].q_above_threshold, 0.0), "RL-State leaves sharing Q");

    rl_apply_reward(bitset, &bitset->sets[0], MESI_MODIFIED, SHARE_ABOVE, 1.0);
    ASSERT_TRUE(nearly_equal(bitset->sets[0].q_modified, 0.0), "RL-Bitset leaves state Q");
    ASSERT_TRUE(nearly_equal(bitset->sets[0].q_above_threshold, 0.5), "RL-Bitset updates sharing Q");

    rl_apply_reward(both, &both->sets[0], MESI_MODIFIED, SHARE_ABOVE, 1.0);
    ASSERT_TRUE(nearly_equal(both->sets[0].q_modified, 0.5), "RL-Both updates state Q");
    ASSERT_TRUE(nearly_equal(both->sets[0].q_above_threshold, 0.5), "RL-Both updates sharing Q");

    freeCache(state);
    freeCache(bitset);
    freeCache(both);
    return 0;
}

static int test_lru_second_chance(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 0.0, 4);
    CacheSet *set = &cache->sets[0];
    ASSERT_TRUE(cache != NULL, "cache allocation");

    accessCacheWithMetadata(cache, 0x0, false, 0, MESI_MODIFIED, 1, true);
    accessCacheWithMetadata(cache, 0x4, false, 0, MESI_EXCLUSIVE, 1, true);
    set->q_modified = -0.5;
    accessCacheWithMetadata(cache, 0x8, false, 0, MESI_SHARED, 1, true);

    ASSERT_TRUE(cache->rl_stats.second_chances_given == 1, "negative first LRU candidate gets one second chance");
    ASSERT_TRUE(history_find_by_tag(set, 1) >= 0, "LRU evicts second candidate after excluding first");
    ASSERT_TRUE(history_find_by_tag(set, 0) < 0, "first LRU candidate was protected");

    freeCache(cache);
    return 0;
}

static int test_lfu_second_chance(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 3, RL_STATE, 0.5, 0.0, 4);
    CacheSet *set = &cache->sets[0];
    ASSERT_TRUE(cache != NULL, "cache allocation");

    accessCacheWithMetadata(cache, 0x0, false, 0, MESI_MODIFIED, 1, true);
    accessCacheWithMetadata(cache, 0x4, false, 0, MESI_EXCLUSIVE, 1, true);
    accessCacheWithMetadata(cache, 0x4, false, 0, MESI_EXCLUSIVE, 1, true);
    set->q_modified = -0.5;
    accessCacheWithMetadata(cache, 0x8, false, 0, MESI_SHARED, 1, true);

    ASSERT_TRUE(cache->rl_stats.second_chances_given == 1, "negative first LFU candidate gets one second chance");
    ASSERT_TRUE(history_find_by_tag(set, 1) >= 0, "LFU evicts second candidate after excluding first");
    ASSERT_TRUE(history_find_by_tag(set, 0) < 0, "first LFU candidate was protected");

    freeCache(cache);
    return 0;
}

static int test_bitset_and_both_scores(void) {
    Cache *bitset = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_BITSET, 0.5, 0.0, 4);
    Cache *both = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_BOTH, 0.5, 0.0, 4);
    ASSERT_TRUE(bitset != NULL && both != NULL, "cache allocation");

    accessCacheWithMetadata(bitset, 0x0, false, 0, MESI_MODIFIED, 1, true);
    accessCacheWithMetadata(bitset, 0x4, false, 0, MESI_EXCLUSIVE, 4, true);
    bitset->sets[0].q_below_threshold = -0.5;
    accessCacheWithMetadata(bitset, 0x8, false, 0, MESI_SHARED, 1, true);
    ASSERT_TRUE(bitset->rl_stats.second_chances_given == 1, "RL-Bitset uses sharing category score");

    accessCacheWithMetadata(both, 0x0, false, 0, MESI_MODIFIED, 1, true);
    accessCacheWithMetadata(both, 0x4, false, 0, MESI_EXCLUSIVE, 4, true);
    both->sets[0].q_modified = 0.5;
    both->sets[0].q_below_threshold = -1.0;
    accessCacheWithMetadata(both, 0x8, false, 0, MESI_SHARED, 1, true);
    ASSERT_TRUE(both->rl_stats.second_chances_given == 1, "RL-Both averages state and sharing scores");

    freeCache(bitset);
    freeCache(both);
    return 0;
}

static int test_epsilon_zero_preserves_behavior(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 0.0, 4);
    CacheSet *set = &cache->sets[0];
    ASSERT_TRUE(cache != NULL, "cache allocation");

    accessCacheWithMetadata(cache, 0x0, false, 0, MESI_MODIFIED, 1, true);
    accessCacheWithMetadata(cache, 0x4, false, 0, MESI_EXCLUSIVE, 1, true);
    set->q_modified = -0.5;
    accessCacheWithMetadata(cache, 0x8, false, 0, MESI_SHARED, 1, true);

    ASSERT_TRUE(cache->rl_stats.second_chances_given == 1, "epsilon 0: negative candidate still gets second chance");
    ASSERT_TRUE(history_find_by_tag(set, 0) < 0, "epsilon 0: first LRU candidate was protected");
    ASSERT_TRUE(cache->rl_stats.exploration_decisions == 0, "epsilon 0 never explores");
    ASSERT_TRUE(cache->rl_stats.rl_decisions == 1, "one real RL decision counted");
    ASSERT_TRUE(cache->rl_stats.exploitation_decisions == 1, "the RL decision was exploitation");

    freeCache(cache);
    return 0;
}

static int test_exploration_occurs(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 0.5, 4);
    ASSERT_TRUE(cache != NULL, "cache allocation");

    srand(1234);
    for (unsigned int i = 0; i < 50; i++) {
        accessCacheWithMetadata(cache, i * 4, false, 0, MESI_MODIFIED, 1, true);
    }

    ASSERT_TRUE(cache->rl_stats.rl_decisions == 48, "48 replacement decisions after 2 fills");
    ASSERT_TRUE(cache->rl_stats.exploration_decisions > 0, "exploration occurs with epsilon 0.5");
    ASSERT_TRUE(cache->rl_stats.exploitation_decisions > 0, "exploitation also occurs with epsilon 0.5");
    ASSERT_TRUE(cache->rl_stats.rl_decisions ==
                cache->rl_stats.exploration_decisions + cache->rl_stats.exploitation_decisions,
                "rl_decisions equals exploration plus exploitation");
    ASSERT_TRUE(cache->rl_stats.exploration_decisions ==
                cache->rl_stats.exploration_accepts + cache->rl_stats.exploration_protects,
                "exploration splits into accepts plus protects");

    freeCache(cache);
    return 0;
}

static int test_epsilon_one_always_explores(void) {
    Cache *cache = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_STATE, 0.5, 1.0, 4);
    ASSERT_TRUE(cache != NULL, "cache allocation");

    srand(1234);
    for (unsigned int i = 0; i < 50; i++) {
        accessCacheWithMetadata(cache, i * 4, false, 0, MESI_MODIFIED, 1, true);
    }

    ASSERT_TRUE(cache->rl_stats.rl_decisions == 48, "48 replacement decisions after 2 fills");
    ASSERT_TRUE(cache->rl_stats.exploration_decisions == cache->rl_stats.rl_decisions,
                "epsilon 1 explores on every RL decision");
    ASSERT_TRUE(cache->rl_stats.exploitation_decisions == 0, "epsilon 1 never exploits");

    freeCache(cache);
    return 0;
}

static Cache *parse_config_from_string(const char *content) {
    const char *path = "rl_unit_tests_tmp_config.txt";
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return NULL;
    }
    fputs(content, file);
    fclose(file);

    file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }
    Cache *cache = parse_config(file);
    fclose(file);
    remove(path);
    return cache;
}

#define BASE_CONFIG "16\n2\n6\n3\n2\n1\n"

static int test_config_epsilon_validation(void) {
    Cache *cache = NULL;

    ASSERT_TRUE(parse_config_from_string(BASE_CONFIG "1\n0.5\n-0.1\n4\n") == NULL, "epsilon -0.1 rejected");
    ASSERT_TRUE(parse_config_from_string(BASE_CONFIG "1\n0.5\n1.1\n4\n") == NULL, "epsilon 1.1 rejected");
    ASSERT_TRUE(parse_config_from_string(BASE_CONFIG "1\n0.5\nabc\n4\n") == NULL, "epsilon abc rejected");

    cache = parse_config_from_string(BASE_CONFIG "1\n0.5\n0\n4\n");
    ASSERT_TRUE(cache != NULL, "epsilon 0 accepted");
    ASSERT_TRUE(nearly_equal(cache->rl_epsilon, 0.0), "epsilon 0 parsed");
    ASSERT_TRUE(cache->num_cores == 4, "num_cores parsed after epsilon");
    freeCache(cache);

    cache = parse_config_from_string(BASE_CONFIG "1\n0.5\n0.1\n4\n");
    ASSERT_TRUE(cache != NULL, "epsilon 0.1 accepted");
    ASSERT_TRUE(nearly_equal(cache->rl_epsilon, 0.1), "epsilon 0.1 parsed");
    freeCache(cache);

    cache = parse_config_from_string(BASE_CONFIG "1\n0.5\n1\n4\n");
    ASSERT_TRUE(cache != NULL, "epsilon 1 accepted");
    ASSERT_TRUE(nearly_equal(cache->rl_epsilon, 1.0), "epsilon 1 parsed");
    freeCache(cache);

    /* legacy 3-optional-line config: third line is num_cores, epsilon defaults to 0 */
    cache = parse_config_from_string(BASE_CONFIG "1\n0.5\n4\n");
    ASSERT_TRUE(cache != NULL, "legacy 3-line RL config accepted");
    ASSERT_TRUE(nearly_equal(cache->rl_epsilon, 0.0), "legacy config defaults epsilon to 0");
    ASSERT_TRUE(cache->num_cores == 4, "legacy config third line is num_cores");
    freeCache(cache);

    return 0;
}

static int test_rl_disabled_unaffected_by_epsilon(void) {
    Cache *plain = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_DISABLED, 0.5, 0.0, 4);
    Cache *eps = initCacheWithRL(16, 2, 6, 3, 2, 1, RL_DISABLED, 0.5, 1.0, 4);
    ASSERT_TRUE(plain != NULL && eps != NULL, "cache allocation");

    srand(1234);
    for (unsigned int i = 0; i < 50; i++) {
        accessCacheWithMetadata(plain, i * 4, false, 0, MESI_MODIFIED, 1, true);
    }
    srand(1234);
    for (unsigned int i = 0; i < 50; i++) {
        accessCacheWithMetadata(eps, i * 4, false, 0, MESI_MODIFIED, 1, true);
    }

    ASSERT_TRUE(plain->stats->num_of_hits == eps->stats->num_of_hits, "RL disabled: hits unaffected by epsilon");
    for (unsigned int way = 0; way < plain->num_ways; way++) {
        ASSERT_TRUE(getTagValue(plain->sets[0].lines[way].tag) == getTagValue(eps->sets[0].lines[way].tag),
                    "RL disabled: victim selection unaffected by epsilon");
    }
    ASSERT_TRUE(eps->rl_stats.rl_decisions == 0, "RL disabled counts no RL decisions");
    ASSERT_TRUE(eps->rl_stats.exploration_decisions == 0, "RL disabled never explores");
    ASSERT_TRUE(eps->rl_stats.exploitation_decisions == 0, "RL disabled never exploits");

    freeCache(plain);
    freeCache(eps);
    return 0;
}

int main(void) {
    ASSERT_TRUE(test_history_capacity_and_invalid_fill() == 0, "history capacity and invalid fill");
    ASSERT_TRUE(test_history_helpers_and_positive_reward() == 0, "history helpers and positive reward");
    ASSERT_TRUE(test_negative_reward_once_and_q_update() == 0, "negative reward and Q update");
    ASSERT_TRUE(test_rl_modes_update_scope() == 0, "RL mode update scope");
    ASSERT_TRUE(test_lru_second_chance() == 0, "LRU second chance");
    ASSERT_TRUE(test_lfu_second_chance() == 0, "LFU second chance");
    ASSERT_TRUE(test_bitset_and_both_scores() == 0, "RL-Bitset and RL-Both scores");
    ASSERT_TRUE(test_epsilon_zero_preserves_behavior() == 0, "epsilon 0 preserves behavior");
    ASSERT_TRUE(test_exploration_occurs() == 0, "exploration occurs");
    ASSERT_TRUE(test_epsilon_one_always_explores() == 0, "epsilon 1 always explores");
    ASSERT_TRUE(test_config_epsilon_validation() == 0, "config epsilon validation");
    ASSERT_TRUE(test_rl_disabled_unaffected_by_epsilon() == 0, "RL disabled unaffected by epsilon");
    printf("rl_unit_tests: PASS\n");
    return 0;
}
