#ifndef __ALIGNER_CHAINER_HPP__
#define __ALIGNER_CHAINER_HPP__

#include "alignment.hpp"


namespace mtg {
namespace graph {
namespace align {

class IDBGAligner;

typedef std::vector<std::pair<Alignment, int64_t>> Chain;
typedef Alignment::score_t score_t;

template <class It = Chain::iterator, class Transformer = FirstDereferencer<Chain::value_type>>
inline size_t get_num_char_matches_in_chain(It begin, It end) {
    return get_num_char_matches_in_seeds<It, Transformer>(begin, end);
}

// Given forward and reverse-complement seeds, construct and call chains of seeds
std::pair<size_t, size_t>
call_seed_chains_both_strands(const IDBGAligner &aligner,
                              std::string_view forward,
                              std::string_view reverse,
                              const DBGAlignerConfig &config,
                              std::vector<Seed>&& fwd_seeds,
                              std::vector<Seed>&& bwd_seeds,
                              const std::function<void(Chain&&, score_t)> &callback,
                              const std::function<bool(Alignment::Column)> &skip_column
                                  = [](Alignment::Column) { return false; });

// Given a set of local alignments, use sparse dynamic programming to construct
// longer alignments, potentially with gaps.
template <class AlignmentCompare>
std::vector<Alignment> chain_alignments(const IDBGAligner &aligner,
                                        std::vector<Alignment>&& alignments);

} // namespace align
} // namespace graph
} // namespace mtg

#endif // __ALIGNER_CHAINER_HPP__
