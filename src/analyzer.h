#ifndef __ANALYZE_H_
#define __ANALYZE_H_

#include <string>
#include <map>
#include <vector>
#include "dependencies/mtwist.h"

#include "config_dynamic.h"
#include "network.h"

extern volatile int CTRL_C_ATTEMPTS;

struct AnalysisStats {
    double prob_add;
    double prob_follow;
    double prob_tweet;
    double prob_norm;

    int64 n_steps, n_follows, n_tweets, n_retweets;
    double event_rate;
    AnalysisStats() {
        prob_add = 0;
        prob_follow = 0;
        prob_tweet = 0;
        prob_norm = 0;

        n_steps = 0, n_follows = 0, n_tweets = 0, n_retweets = 0;
        event_rate = 0;
    }
};


const int APPROX_MONTH = 24 * 60 * 30;

// All the state passed to - and - from analyze.cpp.
// Essentially this encapsulates all the information required for the post-analysis routines.
// This is 'conceptually cleaner' than passing along the entire contents of the Analyzer struct.

struct AnalysisState {
    ParsedConfig config;

    // Note, 'MemPoolVectorGrower' is required to be in same scope as network, otherwise we will have undefined memory accesses.
    MemPoolVectorGrower<int> follow_set_grower;
    // The full contents of the simulated network.
    Network network;
    // Various categorizations of users.
    CategoryGrouper tweet_ranks;
    CategoryGrouper follow_ranks;
    CategoryGrouper retweet_ranks;
    CategoryGrouper age_ranks;

    // Our distinct entity classes.
    // Entity probabilities are derived from config,
    // while the list of users within is derived from
    EntityTypeVector entity_types;
    std::vector<int> entity_cap;
    // Add any values that must be extracted from 'analyze' here.
    int n_follows;
    double r_follow_norm, end_time;

    MTwist rng;

    AnalysisStats stats;
    // The current simulation time
    double time;
    AnalysisState(const ParsedConfig& config, int seed) :
            config(config) {
        n_follows = 0;
        r_follow_norm = end_time = 0;
        tweet_ranks = config.tweet_ranks;
        follow_ranks = config.follow_ranks;
        retweet_ranks = config.retweet_ranks;
        entity_types = config.entity_types;

        rng.init_genrand(seed);
        time = 0.0;
        // Let analyze.cpp handle any additional initialization logic from here.
    }

    int n_months() {
        return time / APPROX_MONTH;
    }

};



enum SelectionType {
    FOLLOW_SELECT,
    RETWEET_SELECT,
    TWEET_SELECT
};
// 'analyzer_select_entity' and 'analyzer_set_rates' implement time-dependent rates
// Select based on any SelectionType
int analyzer_select_entity(AnalysisState& state, SelectionType type);
void analyzer_rate_update(AnalysisState& state);

// Run a network simulation using the given input file's parameters
void analyzer_main(AnalysisState& analysis_state);

#endif