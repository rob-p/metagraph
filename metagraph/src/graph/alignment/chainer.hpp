#ifndef __ALIGN_CHAIN__
#define __ALIGN_CHAIN__

#include "graph/alignment/alignment.hpp"

namespace mtg::graph::align {

template <typename Anchor>
using ChainScores = std::vector<std::tuple<score_t, const Anchor*, size_t>>;

using AlignmentCallback = std::function<void(Alignment&&)>;

template <typename Anchor>
using AnchorConnector = std::function<void(const Anchor&, // start anchor
                                           const Anchor*, // target anchors begin
                                           const Anchor*, // target anchors end
                                           typename ChainScores<Anchor>::pointer,
                                           const std::function<bool(score_t,       // connect score
                                                                    const Anchor*, // last
                                                                    size_t         // distance
                                                                   )>&
                                          )>;

template <typename Anchor>
using AnchorExtender = std::function<void(const Anchor*, // first ptr,
                                          Alignment&&,   // target,
                                          size_t,        // distance,
                                          const AlignmentCallback&)>;

template <typename Anchor>
using BacktrackStarter = std::function<bool(const std::vector<std::pair<const Anchor*, size_t>>&,
                                            score_t)>;

template <typename Anchor>
void chain_anchors(const DBGAlignerConfig &config,
                   const Anchor *anchors_begin,
                   const Anchor *anchors_end,
                   const AnchorConnector<Anchor> &anchor_connector,
                   const BacktrackStarter<Anchor> &start_backtrack
                       = [](const auto&, score_t) { return true; },
                   const AnchorExtender<Anchor> &anchor_extender
                       = [](const auto*, auto&&, size_t, const auto&) {},
                   const AlignmentCallback &callback = [](auto&&) {},
                   const std::function<bool()> &terminate = []() { return false; },
                   bool allow_overlap = false,
                   ssize_t max_gap_between_anchors = 100,
                   ssize_t max_gap_shrink_factor = 4) {
    if (terminate() || anchors_begin == anchors_end)
        return;

    ssize_t query_size = anchors_begin->get_clipping() + anchors_begin->get_end_clipping()
                            + anchors_begin->get_query_view().size();

    assert(std::is_sorted(anchors_begin, anchors_end, [&](const auto &a, const auto &b) {
        return std::make_pair(b.get_orientation(), a.get_query_view().end())
             > std::make_pair(a.get_orientation(), b.get_query_view().end());
    }));

    const Anchor *orientation_change = anchors_end;
    ChainScores<Anchor> chain_scores;
    chain_scores.reserve(anchors_end - anchors_begin);
    for (auto it = anchors_begin; it != anchors_end; ++it) {
        chain_scores.emplace_back(it->get_score(config), anchors_end, it->get_clipping());
        if (it != anchors_begin && (it - 1)->get_orientation() != it->get_orientation())
            orientation_change = it;
    }

    // forward pass
    max_gap_between_anchors = std::min(max_gap_between_anchors, query_size);
    auto forward_pass = [&](const Anchor *anchors_begin,
                            const Anchor *anchors_end,
                            auto *chain_scores) {
        if (anchors_begin == anchors_end)
            return;

        ssize_t b = max_gap_between_anchors;
        ssize_t b_last;
        do {
            auto j = anchors_begin;
            for (auto i = anchors_begin + !allow_overlap; i != anchors_end; ++i) {
                auto end = i->get_query_view().end();
                while (j != anchors_end && j->get_query_view().end() - end > b) {
                    ++j;
                }

                auto i_end = i;
                if (allow_overlap) {
                    while (i_end != anchors_end && i_end->get_query_view().end() == end) {
                        ++i_end;
                    }
                }

                auto [max_score, best_last, best_dist] = chain_scores[i - anchors_begin];
                max_score = std::numeric_limits<score_t>::min();
                best_last = anchors_end;
                best_dist = i->get_clipping();

                // align anchor i forwards
                anchor_connector(*i, j, i_end, chain_scores + (j - anchors_begin),
                    [&](score_t score, const Anchor* last, size_t dist) {
                        assert(last != i);
                        if (std::tie(score, best_dist) > std::tie(max_score, dist)) {
                            max_score = score;
                            best_last = last;
                            best_dist = dist;
                            return true;
                        }

                        return false;
                    }
                );

                if (max_score > std::numeric_limits<score_t>::min()) {
                    auto &cur_scores = chain_scores[i - anchors_begin];
                    bool updated = (max_score > std::get<0>(cur_scores));
                    cur_scores = std::tie(max_score, best_last, best_dist);
                    if (allow_overlap && updated) {
                        while (i >= anchors_begin && i->get_query_view().end() == end) {
                            --i;
                        }
                    }
                }
            }
            b_last = b;
            b *= max_gap_shrink_factor;
        } while (std::get<0>(chain_scores[anchors_end - anchors_begin - 1])
                    < query_size - b_last / 2);
    };

    forward_pass(anchors_begin, orientation_change, chain_scores.data());
    forward_pass(orientation_change, anchors_end,
                 chain_scores.data() + (orientation_change - anchors_begin));

    // backtracking
    std::vector<std::pair<score_t, size_t>> best_chains;
    best_chains.reserve(chain_scores.size());
    for (size_t i = 0; i < chain_scores.size(); ++i) {
        const auto &[score, last, dist] = chain_scores[i];

        if (score > 0)
            best_chains.emplace_back(-score, i);
    }

    std::sort(best_chains.begin(), best_chains.end());

    sdsl::bit_vector used(chain_scores.size());
    for (auto [nscore, i] : best_chains) {
        if (terminate())
            return;

        if (used[i])
            continue;

        std::vector<std::pair<const Anchor*, size_t>> chain;
        const auto *last_anchor = anchors_begin + i;
        chain.emplace_back(last_anchor, 0);
        auto [score, last, dist] = chain_scores[i];
        while (last != anchors_end) {
            last_anchor = last;
            size_t to_traverse = dist;
            std::tie(score, last, dist) = chain_scores[last - anchors_begin];
            chain.emplace_back(last_anchor, to_traverse);
        }

        if (!start_backtrack(chain, -nscore))
            continue;

        for (const auto &[a_ptr, dist] : chain) {
            used[a_ptr - anchors_begin] = true;
        }

        std::vector<Alignment> alns;
        alns.emplace_back(*chain.back().first, config);
        for (auto it = chain.rbegin(); it + 1 != chain.rend(); ++it) {
            std::vector<Alignment> next_alns;
            for (auto&& aln : alns) {
                anchor_extender((it + 1)->first, std::move(aln), it->second,
                    [&](Alignment&& next_aln) {
                        next_alns.emplace_back(std::move(next_aln));
                    }
                );
            }
            std::swap(next_alns, alns);
        }

        for (auto&& aln : alns) {
            if (terminate())
                return;

            callback(std::move(aln));
        }
    }
}

} // namespace mtg::graph::align

#endif // __ALIGN_CHAIN__