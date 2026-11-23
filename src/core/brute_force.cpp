#include "plgen/brute_force.hpp"

#include <algorithm>

#include "plgen/distance.hpp"

namespace plgen {

std::vector<Neighbor> brute_force_knn(const FeatureStore& store, VecView query, std::size_t k,
                                      std::optional<SongId> exclude) {
  // Never trust k: it will come from an HTTP query string. Capping it also
  // keeps reserve() below from trying to allocate gigabytes.
  k = std::min(k, store.size());
  if (k == 0) return {};

  // `heap` holds the k best candidates seen so far, arranged as a MAX-heap by
  // distance: heap.front() is the WORST of the current best k — exactly the one
  // each new candidate has to beat.
  //
  // std::push_heap / pop_heap keep the "largest" element (per the comparator)
  // at the front. With comparator closer(a, b) == "a ranks before b", the
  // largest is whichever ranks LAST, i.e. the farthest. So `closer` => max-heap.
  std::vector<Neighbor> heap;
  heap.reserve(k);

  const auto n = static_cast<SongId>(store.size());
  for (SongId id = 0; id < n; ++id) {
    if (exclude && *exclude == id) continue;
    const Neighbor cand{id, squared_l2(query, store.vector(id))};

    if (heap.size() < k) {
      heap.push_back(cand);
      std::push_heap(heap.begin(), heap.end(), closer);  // sift the new element up
    } else if (closer(cand, heap.front())) {
      std::pop_heap(heap.begin(), heap.end(), closer);   // move worst to the back
      heap.back() = cand;                                // overwrite it
      std::push_heap(heap.begin(), heap.end(), closer);  // restore heap order
    }
  }

  // Turns a heap into a sorted range in O(k log k): closest first.
  std::sort_heap(heap.begin(), heap.end(), closer);
  return heap;
}

}  // namespace plgen
