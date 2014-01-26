#ifndef CAT_NODES_H_
#define CAT_NODES_H_

#include <vector>

#include <cstdio>
#include <cmath>
#include "util.h"
#include "dependencies/mtwist.h"
#include "lcommon/typename.h"

#include <google/sparse_hash_set>

namespace cats {

/* Adding to a category group defined recursively.
 * Templates allow us to nest, eg TreeNode<TreeNode<LeafNode>> represents a twice-nested category.
 * A guiding, generic, 'data structure', ClassifierT, is used recursively as well. To provide a ClassifierT structure,
 * create a structure like follows:
 *
 *  struct InnerClassifier {
 *      int classify(Network& N, Foo bar) {
 *          ... return inner category number ...
 *      }
 *      double get(Network& N, int bin) { return rates[bin]; }
 *      // The rates to use
 *      std::vector<double> rates;
 *  };
 *
 *  struct OuterClassifier {
 *      int classify(Network& N, Foo bar) {
 *          ... return outer category number ...
 *      }
 *      InnerClassifier& get(Network& N, int bin) { return children[bin]; }
 *      std::vector<InnerClassifier> children;
 *  };
 *
 *  Then, pass along an instance of OuterLayer when adding to the structure.
 */

/* Every LeafNode holds a google-spare-hash-set. These data structures are highly memory efficient.
 * They are also fairly dynamic. If we hit memory problems, we can write to disk and restart the simulation.
 * The simulation will then be 'defragmented'.
 * For this reason the nice properties of dynamic memory allocation (simplicity and very decent performance) were preferred. */
template <typename ElemT>
struct LeafNode {
    typedef google::sparse_hash_set<ElemT> HashSet;

    struct iterator {
        int slot;
        iterator() : slot(0) {
        }
    };

    bool iterate(iterator& iter, ElemT& elem) {
        int& slot = iter.slot;
        int n_hash_slots = elems.rep.table.size();
        if (slot >= n_hash_slots) {
            // At end, leave
            return false;
        }
        // If not at end, we will loop until we find a hash-slot that
        // contains a valid instance of 'ElemT'.
        while (slot < n_hash_slots && !elems.rep.table.test(slot)) {
            slot++;
        }
        if (slot >= n_hash_slots) {
            // At end, leave
            return false;
        }
        elem = elems.rep.table.unsafe_get(slot);
        slot++; // Move to next unused slot
        return true;
    }

    LeafNode() {
        total_rate = 0;
        // MUST be done to use erase() with Google's HashSet
        elems.set_deleted_key(ElemT(-1));
    }

    int size() {
        return elems.size();
    }

    template <typename StateT>
    bool add(StateT& N, double rate, double& delta, const ElemT& elem) {
        if (insert(elem)) { // Did we insert a unique element?
            delta = rate;
            total_rate += delta;
            return true;
        }
        return false;
    }

    template <typename StateT>
    bool add(StateT& N, double rate, const ElemT& elem) {
        double dummy;
        return add(N, rate, dummy, elem);
    }

    template <typename StateT>
    bool remove(StateT& N, double rate, double& delta, const ElemT& elem) {
        if (elems.erase(elem) > 0) { // Did we erase an element that was in our set?
            delta = -rate;
            total_rate += delta;
            return true;
        }
        return false;
    }

    template <typename StateT>
    bool remove(StateT& N, double rate, const ElemT& elem) {
        double dummy;
        return remove(N, rate, dummy, elem);
    }

    double get_total_rate() {
        return total_rate;
    }

    template <typename StateT>
    double recalc_rates(StateT& N, double rate) {
        total_rate = rate * elems.size();
        return total_rate;
    }

    template <typename StateT>
    void print(StateT& N, double rate, int bin, int layer) {
        for (int i = 0; i < layer; i++) {
            printf("  ");
        }
        printf("[Bin %d][leaf] (Total %.2f; N_elems %d; Rate %.2f) ", bin, float(total_rate), rate, size());
        printf("[");
        typename HashSet::iterator it = elems.begin();
        for (; it != elems.end(); ++it) {
            printf("%d ", *it);
        }
        printf("]\n");
    }

    template <typename StateT, typename ClassifierT, typename Node>
    void transfer(StateT& N, ClassifierT& C, Node& o) {
        typename HashSet::iterator it = elems.begin();
        for (; it != elems.end(); ++it) {
            double delta = 0.0;
            o.add(N, C, delta, *it);
        }
        elems.clear();
    }

    bool pick_random_uniform(MTwist& rng, ElemT& elem) {
        if (elems.empty()) {
            return false;
        }
        int n_hash_slots = elems.rep.table.size();
        int idx;
        do {
            // We will loop until we find a hash-slot that
            // contains a valid instance of 'ElemT'.
            idx = rng.rand_int(n_hash_slots);
        } while (!elems.rep.table.test(idx));
        elem = elems.rep.table.unsafe_get(idx);
        return true;
    }

    bool pick_random_weighted(MTwist& rng, ElemT& elem) {
        // Same thing for LeafNode's
        return pick_random_uniform(rng, elem);
    }
private:
    double total_rate;
    HashSet elems;

    // Returns true if the element was unique
    bool insert(const ElemT& elem) {
        size_t prev_size = elems.size();
        elems.insert(elem);
        return (elems.size() > prev_size);
    }
};

/* Points either to further TreeNode's, or a LeafNode. */
template <typename SubCat>
struct TreeNode {
    typedef typename SubCat::iterator sub_iterator;
    struct iterator {
        int bin;
        sub_iterator sub_iter;
        iterator() {
            bin = 0;
        }
    };

    template <typename ElemT>
    bool iterate(iterator& iter, ElemT& elem) {
        int& bin = iter.bin;
        while (bin < cats.size()) {
            if (cats[bin].iterate(iter.sub_iter, elem)) {
                return true;
            }
            bin++;
            iter.sub_iter = sub_iterator();
        }
        return false;
    }

    TreeNode() {
        total_rate = 0;
        n_elems = 0;
    }
    int size() {
        return n_elems;
    }

    void swap(TreeNode& o) {
        double tmp_total = total_rate;
        int tmp_n = n_elems;
        n_elems = o.n_elems;
        total_rate = o.total_rate;
        o.n_elems = tmp_n;
        o.total_rate = tmp_total;
        cats.swap(o.cats);
    }

    /* Useful for time-dependent rates */
    template <typename StateT, typename ClassifierT>
    void shift_and_recalc_rates(StateT& S, ClassifierT& C) {
        cats.resize(cats.size() + 1);
        // Empty category should be swapped into first slot by end
        for (int i = cats.size() - 1; i >= 1; i--) {
            cats[i].swap(cats[i-1]);
        }
        // Collapse all categories that are more than the intended maximum
        int max = C.size();
        for (int i = max; i < cats.size(); i++) {
            cats[i].transfer(S, C.get(S, max - 1), cats[max - 1]);
        }
        cats.resize(C.size());
        recalc_rates(S, C);
    }

    template <typename StateT, typename ClassifierT, typename Node>
    void transfer(StateT& S, ClassifierT& C, Node& o) {
        for (int i = 0; i < cats.size(); i++) {
            cats[i].transfer(S, C, o);
        }
        cats.clear();
    }

    template <typename StateT, typename ClassifierT>
    double recalc_rates(StateT& S, ClassifierT& C) {
        total_rate = 0;
        for (int i = 0; i < n_bins(); i++) {
            total_rate += cats[i].recalc_rates(S, C.get(S,i));
        }
        return total_rate;
    }

    // Leaf node, return true if the element already existed
    template <typename StateT, typename ClassifierT, typename ElemT>
    bool add(StateT& S, ClassifierT& C, double& ret, const ElemT& elem) {
        double delta = 0.0;
        int bin = C.classify(S, elem);
        ensure_bin(bin);
        if (cats[bin].add(S, C.get(S, bin), delta, elem)) { // Did we insert a unique element?
            n_elems++;
            DEBUG_CHECK(delta >= 0, "Negative rate delta on 'add'!");
            total_rate += delta;
            ret = delta;
            return true;
        }
        return false;
    }

    template <typename StateT, typename ElemT>
    bool add(StateT& N, double rate, const ElemT& elem) {
        double dummy;
        return add(N, rate, dummy, elem);
    }

    template <typename ElemT>
    bool pick_random_uniform(MTwist& rng, ElemT& elem) {
        if (cats.empty()) {
            return false;
        }
        // Same thing for LeafNode's
        SubCat& sub_cat = cats[random_uniform_bin(rng)];
        return sub_cat.pick_random_uniform(rng, elem);
    }

    template<typename ElemT>
    bool pick_random_weighted(MTwist& rng, ElemT& elem) {
        if (cats.empty()) {
            return false;
        }
        // Same thing for LeafNode's
        SubCat& sub_cat = cats[random_weighted_bin(rng)];
        return sub_cat.pick_random_weighted(rng, elem);
    }

    // Leaf node, return true if the element was actually in the set
    template <typename StateT, typename ClassifierT, typename ElemT>
    bool remove(StateT& S, ClassifierT& C, double& ret, const ElemT& elem) {
        double delta = 0.0;
        int bin = C.classify(S, elem);
        ensure_bin(bin);
        if (cats[bin].remove(S, C.get(S, bin), delta, elem)) { // Did we insert a unique element?
            n_elems--;
            DEBUG_CHECK(delta <= 0, "Positive rate delta on 'remove'!");
            total_rate += delta;
            ret = delta;
            return true;
        }
        return false;
    }

    template <typename StateT, typename ElemT>
    bool remove(StateT& N, double rate, const ElemT& elem) {
        double dummy;
        return remove(N, rate, dummy, elem);
    }

    void ensure_bin(int bin) {
        if (bin >= cats.size()) {
            cats.resize(bin + 1);
        }
    }

    /* Uniform method, choose with respect to amount only. */
    int random_uniform_bin(MTwist& rng) {
        int num = rng.rand_int(size());
        for (int i = 0; i < cats.size(); i++) {
            num -= cats.size();
            if (num <= 0) {
                return i;
            }
        }
        ASSERT(false, "Logic error! No bin to choose from, or n_elems don't add up.");
        return -1;
    }
    /* Principal KMC method, choose with respect to bin rates. */
    int random_weighted_bin(MTwist& rng) {
        double num = rng.genrand_real2() * total_rate;
        for (int i = 0; i < cats.size(); i++) {
            num -= cats[i].get_total_rate();
            if (num <= 0) {
                return i;
            }
        }
        ASSERT(!cats.empty(), "Logic error! No bin to choose from.");
        return cats.size() - 1; // Assume we should choose last one due to floating point issues
    }

    size_t n_bins() {
        return cats.size();
    }

    SubCat& operator[](int bin) {
        return cats[bin];
    }

    double get_total_rate() {
        return total_rate;
    }

    template <typename StateT, typename ClassifierT>
    void print(StateT& S, ClassifierT& C, int bin = 0, int layer = 0) {
        for (int i = 0; i < layer; i++) {
            printf("  ");
        }
        printf("[Bin %d][%s] (Total %.2f; N_elems %d) \n",
                bin,
                cpp_type_name_no_namespace(C).c_str(),
                float(total_rate),
                size()
        );
        for (int i = 0; i < cats.size(); i++) {
            cats[i].print(S, C.get(S, i), i, layer + 1);
        }
    }
private:
    double total_rate; // Total weight of the subtree
    int n_elems;
    std::vector<SubCat> cats;
};

}

#endif
