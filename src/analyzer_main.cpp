#include <string>
#include <cassert>
#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cmath>
#include <deque>

// Local includes:
#include "analyzer.h"
#include "util.h"
#include "MemPoolVector.h"
#include "network.h"

#include "dependencies/lcommon/Timer.h"

#include <signal.h>

using namespace std;

static const double ZEROTOL = 1e-16; // enough precision for really really low add rate

volatile int CTRL_C_ATTEMPTS = 0;

static const int CTRL_C_ATTEMPTS_TO_ABORT = 4;
// Handler for SIGINT -- sent by Ctrl-C on command-line. Allows us to stop our program gracefully!
static void ctrl_C_handler(int __dummy) {
    CTRL_C_ATTEMPTS++;
    if (CTRL_C_ATTEMPTS > CTRL_C_ATTEMPTS_TO_ABORT) {
        error_exit("User demands abort!");
    }
}
static void ctrl_C_handler_install() {
   signal(SIGINT, ctrl_C_handler);
}

/* The Analyze struct encapsulates the many-parameter analyze function, and its state. */
struct Analyzer {

    /***************************************************************************
     * Configuration and state variables of the analysis routines.
     ***************************************************************************/

    AnalysisState& state;
    ParsedConfig& config;
    AnalysisStats& stats;

    // struct for the entity classes, see network.h for specifications
    EntityTypeVector& entity_types;

    // The network state
    Network& network;
    MemPoolVectorGrower<int>& follow_set_grower;

	// Categories for tweeting, following, retweeting
    CategoryGrouper& tweet_ranks;
	CategoryGrouper& follow_ranks;
	CategoryGrouper& retweet_ranks;

	vector <double> follow_probabilities, updating_follow_probabilities;
	vector <int> last_entity_ID;

    /* Mersenne-twister random number generator */
    MTwist& rng;

    Timer timer;

    ofstream DATA_TIME; // Output file to plot data

    double& time;

    /***************************************************************************
     * Initialization functions
     ***************************************************************************/

    /* Initialization and loading of configuration.
     * Reads configuration from the given input file. */
    Analyzer(AnalysisState& state) :
        //** Here, we initialize REFERENCES to the various analysis pieces.
        //** The only way to initialize references is through this construct -- called an 'initializer list'.
        //** This ensures that updates are witnessed in the caller's AnalysisState object.
            state(state), stats(state.stats), config(state.config),
            entity_types(state.entity_types), network(state.network),
            follow_set_grower(state.follow_set_grower),
            tweet_ranks(state.tweet_ranks), follow_ranks(state.follow_ranks), retweet_ranks(state.retweet_ranks),
            rng(state.rng), time(state.time) {

        // The following allocates a memory chunk proportional to max_entities:
        network.preallocate(config.max_entities);
        follow_set_grower.preallocate(FOLLOW_SET_MEM_PER_USER * config.max_entities);

        DATA_TIME.open("DATA_vs_TIME");

		set_initial_entities();
		analyzer_rate_update(state);
    }
    // make sure any initial entities are given a title based on the respective probabilities
    void set_initial_entities() {
        for (int i = 0; i < config.initial_entities; i++) {
             action_create_entity(0.0, i);
        }
    }

	 /***************************************************************************
     * Entity mutation routines
     ***************************************************************************/
    // Return whether the follow could be added, or if we have run out of buffer room.
    bool handle_follow(int actor, int target) {
        Entity& A = network[actor];
        Entity& T = network[target];
        bool was_added = follow_set_grower.add_if_possible(A.follow_set, target);
        if (was_added) {
            was_added = follow_set_grower.add_if_possible(T.follower_set, actor);
            return true;
        }
        return false;
    }
    /***************************************************************************
     * Analysis routines
     ***************************************************************************/

    // Entry point to all analysis:
    /* Conceptually this is the true entry point to our program,
     * after the messy configuration and allocation is done.
     * Returns end-time. */
    double main() {
        run_network_simulation();
        return time;
    }

    // ROOT ANALYSIS ROUTINE
    /* Run the main analysis routine using this config. */
    void run_network_simulation() {
        ctrl_C_handler_install();
        while ( LIKELY(time < config.max_time && network.n_entities < config.max_entities && CTRL_C_ATTEMPTS == 0) ) {
        	step_analysis();
        }
    }

    void step_time(int n_entities) {
        double prev_milestone = floor(time / TIME_CAT_FREQ);
        double prev_integer = floor(time);
        if (config.use_random_increment == 1) {
            // increment by random time
            time += -log(rng.rand_real_not0()) / stats.event_rate;
        } else {
            time += 1 / stats.event_rate;
        }

        // Categorize all entities based on time, on every new time milestone.
        bool at_milestone = (floor(time / TIME_CAT_FREQ) > prev_milestone);
        if (at_milestone){
        	last_entity_ID.push_back(n_entities);
        }

        if (config.output_stdout_summary && (floor(time) > prev_integer)) {
            output_summary_stats(time);
        }
    }

    /* Create a entity at the given index.
     * The index should be an empty entity slot. */
    void action_create_entity(double creation_time, int index) {
		Entity& e = network[index];
		e.creation_time = creation_time;
		double rand_num = rng.rand_real_not0();
		for (int et = 0; et < entity_types.size(); et++) {
			if (rand_num <= entity_types[et].prob_add) {
				e.entity = et;
                entity_types[et].entity_list.push_back(index);
				follow_ranks.categorize(index, e.follower_set.size);
				break;
			}
			rand_num -= entity_types[et].prob_add;
		}
		if (config.use_barabasi){
			action_follow_entity(index, index, creation_time);
		}
        network.n_entities++;
    }

    /* decides which entity to follow based on the rates in the INFILE */
	void action_follow_entity(int entity, int n_entities, double time_of_follow) {
		Entity& e1 = network[entity];
		int entity_to_follow = -1;
		double rand_num = rng.rand_real_not0();
		// if we want to do random follows
		if (config.follow_model == RANDOM_FOLLOW) {
			// find a random entity within [0:number of entities - 1]	
			entity_to_follow = rng.rand_int(n_entities);
		} else if (config.follow_model == PREFERENTIAL_FOLLOW) {
		    // if we want to use a preferential follow method
			double sum_of_weights = 0;
			updating_follow_probabilities.resize(follow_probabilities.size());
			/* search through the probabilities for each threshold and find
			   the right bin to land in */
			for (int i = 0; i < follow_probabilities.size(); i ++){
				// look at each category
				CategoryEntityList& C = follow_ranks.categories[i];
				updating_follow_probabilities[i] = follow_probabilities[i]*C.entities.size();
				sum_of_weights += C.entities.size()*follow_probabilities[i];				
			}
			for (int i = 0; i < follow_probabilities.size(); i ++ ){
				CategoryEntityList& C = follow_ranks.categories[i];
	 		   	updating_follow_probabilities[i] /= sum_of_weights;
			}
			for (int i = 0; i < follow_probabilities.size(); i ++) {
				if (rand_num - updating_follow_probabilities[i] <= ZEROTOL) {
					// point to the category we landed in
					CategoryEntityList& C = follow_ranks.categories[i];
					// make sure we're not pulling a entity from an empty list
					if (C.entities.size() != 0) {
						// pull a random entity from whatever bin we landed in and break so we do not continue this loop
						entity_to_follow = C.entities[rng.rand_int(C.entities.size())];
						break;
					}
				}
				// part of the above search
				rand_num -= updating_follow_probabilities[i];
			}
		} else if (config.follow_model == ENTITY_FOLLOW) {
		    // if we want to follow by entity class
			/* search through the probabilities for each entity and find the right bin to land in */
			for (int i = 0; i < entity_types.size(); i++) {
				if (rand_num <= entity_types[i].prob_follow) {
					// make sure we're not pulling from an empty list
					if (entity_types[i].entity_list.size() != 0) {
						// pull the entity from whatever bin we landed in and break so we dont continue this loop
						entity_to_follow = entity_types[i].entity_list[rng.rand_int(entity_types[i].entity_list.size())];
						break;
					}
				}
				// part of the above search
				rand_num -= entity_types[i].prob_follow;
			}
		} else if (config.follow_model == RETWEET_FOLLOW) {
		    //Retweet follow method
			if (rand_num > 0.5) {
			    Retweet retweet;
			    if (e1.retweets.check_recent(retweet)) {
			        // grab the latest retweet
					// if the retweet happened in the last 48 hours
					if (time_of_follow - retweet.time < 2880) {
						entity_to_follow = retweet.original_tweeter;
					}
				}
			}
			else {
				entity_to_follow = rng.rand_int(n_entities);
			}
		}

		// check and make sure we are not following ourself, or we are following entity -1
		if (LIKELY(entity != entity_to_follow && entity_to_follow != -1)) {
			// point to the entity who is being followed
			if (handle_follow(entity, entity_to_follow)) {
				// based on the number of followers the followed-entity has, check to make sure we're still categorized properly
				Entity& target = network[entity_to_follow];
				follow_ranks.categorize(entity_to_follow, target.follower_set.size);
				stats.n_follows++; // We were able to add the follow; almost always the case.
			}
        } 
        // Otherwise, no follow is added, this is unlikely
	}

	// function to handle the tweeting
	void action_tweet(int entity) {
		// This is the entity tweeting
		Entity& e = network[entity];
		// increase the number of tweets the entity had by one
		e.n_tweets++;
		tweet_ranks.categorize(entity, e.n_tweets);
//		if (e.n_tweets > 1000) {
//			action_unfollow(entity);
//		}
	}
	
	void action_retweet(int entity, double time_of_retweet) {
		Entity& retweetee = network[entity];
		FollowList& f = retweetee.follow_set;
		int entity_retweeted = -1;
		double rand_num = rng.rand_real_not0();
		if (rand_num < 0.5) {
			if (f.size != 0){
				entity_retweeted = f[rng.rand_int(f.size)];
			}
		}
		else {
			// retweet a retweet
		    Retweet retweet;
		    if (retweetee.retweets.check_recent(retweet)) {
		        // grab the latest retweet
				// if the retweet happened in the last 48 hours
				if (time_of_retweet - retweet.time < 2880) {
					entity_retweeted = retweet.original_tweeter;
				}
			}
		}
		if (entity_retweeted != -1 /*No retweet should occur*/) {
			int n_following = network.n_following(entity);
            // Loop over all the entities that witness the event:
			for (int i = 0; i < n_following; i ++){
				Entity& audience = network[network.follow_i(entity,i)];
				Retweet retweet(entity_retweeted, time_of_retweet);
				audience.retweets.add(retweet);
			}
			retweetee.n_retweets ++;
			stats.n_retweets ++;
		}
	}
	
	void action_followback(int follower, int followee) {
		// now the followee will follow the follower back
		if (handle_follow(followee, follower)) {
				Entity& target = network[follower];
				follow_ranks.categorize(follower, target.follower_set.size);
				stats.n_follows++; // We were able to add the follow; almost always the case.
		} 
	}
	
	void action_unfollow(int entity_id) {
	    // Broken, commented out -- AD
//		Entity& e1 = network[entity_id];
//		int unfollowing_location = rng.rand_int(network.n_followers(entity_id));
//		Entity& e2 = network[unfollowing_location];
//		FollowerList& f1 = e1.follower_set;
//		FollowList& f2 = e2.follow_set;
		// remove unfollowing location from follower_list
		// remove entity id from follow_list
		
	}

    // Performs one step of the analysis routine.
    // Takes old time, returns new time
    void step_analysis() {
        double u_1 = rng.rand_real_not0(); // get the first number with-in [0,1).
        double rand_num = rng.rand_real_not0();
        int N = network.n_entities;
        int entity = -1;
        double r_follow_sum = 0, r_tweet_sum = 0, r_retweet_sum = 0;

        // DECIDE WHAT TO DO:
        if (u_1 - (stats.prob_add) <= ZEROTOL) {
                        // If we find ourselves in the add entity chuck of our cumulative function:
            action_create_entity(time, N);
        } else if (u_1 - (stats.prob_add + stats.prob_follow) <= ZEROTOL) {
            int entity = analyzer_select_entity(state, FOLLOW_SELECT);
            action_follow_entity(entity, N, time);
        } else if (u_1 - (stats.prob_add + stats.prob_follow + stats.prob_tweet) <= ZEROTOL) {
                        // The tweet event
            int entity = analyzer_select_entity(state, TWEET_SELECT);
            action_tweet(entity);
            stats.n_tweets ++;
        } else if (u_1 - (stats.prob_add + stats.prob_follow + stats.prob_tweet + stats.prob_norm) <= ZEROTOL ) {
            int entity = analyzer_select_entity(state, RETWEET_SELECT);
            action_retweet(entity, time);
        } else {
            cout << "Disaster, event out of bounds" << endl;
        }

        step_time(N);
        stats.n_steps++;
        //update the rates if n_entities has changed
        analyzer_rate_update(state);

#ifdef SLOW_DEBUG_CHECKS
        static int i = 0;
        if ((i%1000)==0) {
        network.sanity_check();
        }
        i++;
#endif
    }

    /***************************************************************************
     * Helper functions
     ***************************************************************************/

    void output_summary_stats(ostream& stream) {
        stream << fixed << setprecision(2)
                << time << "\t\t"
                << network.n_entities << "\t\t"
                << stats.n_follows << "\t\t"
                << stats.n_tweets << "\t\t"
                << stats.n_retweets << "\t\n";
    }

    void output_summary_stats(double TIME) {
        static int n_outputs = 0;

		const char* HEADER = "\n#Time\t\tUsers\t\tFollows\t\tTweets\t\tRetweets\n\n";
        if (n_outputs % (25 * STDOUT_OUTPUT_RATE) == 0) {
        	cout << HEADER;
        }
        if (n_outputs % 500 == 0) {
            DATA_TIME << HEADER;
        }

        output_summary_stats(DATA_TIME);
        if (n_outputs % STDOUT_OUTPUT_RATE == 0) {
        	output_summary_stats(cout);
        	/*// In standard output only, print the follow lists of the first 10 entities:
        	int capped_len = std::min(10, network.n_entitys);
        	for (int i = 0; i < capped_len; i++) {
        		MemPoolVector& follow_set = network[i].follow_set;
        		cout << (i+1) << ": ";
        		for (int i = 0; i < follow_set.n_following; i++) {
        			cout << ' ' << follow_set[i];
        		}
        		cout << '\n';
        	}*/
        }

        n_outputs++;
    }
};

// Run a network simulation using the given input file's parameters
void analyzer_main(AnalysisState& analysis_state) {
    Analyzer analyzer(analysis_state);
    analyzer.main();
}