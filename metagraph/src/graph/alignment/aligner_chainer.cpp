#include "aligner_chainer.hpp"

#include <x86/svml.h>

#include "aligner_seeder_methods.hpp"
#include "aligner_extender_methods.hpp"
#include "aligner_aggregator.hpp"
#include "aligner_labeled.hpp"

#include "common/utils/simd_utils.hpp"
#include "common/aligned_vector.hpp"
#include "graph/graph_extensions/path_index.hpp"
#include "graph/representation/canonical_dbg.hpp"

namespace mtg {
namespace graph {
namespace align {

using common::logger;

typedef DeBruijnGraph::node_index node_index;

constexpr uint32_t nid = std::numeric_limits<uint32_t>::max();

struct TableElem {
    Alignment::Column label;
    int64_t coordinate;
    int32_t seed_clipping;
    int32_t seed_end;
    score_t chain_score;
    uint32_t current_seed_index;

    TableElem(Alignment::Column c, int64_t coordinate, int32_t seed_clipping,
              int32_t seed_end, score_t chain_score, uint32_t current_seed_index)
          : label(c), coordinate(coordinate), seed_clipping(seed_clipping),
            seed_end(seed_end), chain_score(chain_score), current_seed_index(current_seed_index) {}
} SIMDE_ALIGN_TO_32;
static_assert(sizeof(TableElem) == 32);

inline constexpr bool operator>(const TableElem &a, const TableElem &b) {
    return std::tie(a.label, a.coordinate, a.seed_clipping, a.seed_end)
        > std::tie(b.label, b.coordinate, b.seed_clipping, b.seed_end);
}

typedef AlignedVector<TableElem> ChainDPTable;

std::tuple<ChainDPTable, AlignedVector<int32_t>, size_t, size_t>
chain_seeds(const DBGAlignerConfig &config,
            std::string_view query,
            std::vector<Seed> &seeds);

struct ChainHash {
    inline std::size_t operator()(const Chain &chain) const {
        uint64_t hash = 0;
        for (const auto &[aln, dist] : chain) {
            for (node_index node : aln.get_nodes()) {
                hash ^= node + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            hash ^= dist + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

std::pair<size_t, size_t>
call_seed_chains_both_strands(const IDBGAligner &aligner,
                              std::string_view forward,
                              std::string_view reverse,
                              const DBGAlignerConfig &config,
                              std::vector<Seed>&& fwd_seeds,
                              std::vector<Seed>&& bwd_seeds,
                              const std::function<void(Chain&&, score_t)> &callback,
                              const std::function<bool(Alignment::Column)> &skip_column,
                              const std::function<bool()> &terminate) {
    fwd_seeds.erase(std::remove_if(fwd_seeds.begin(), fwd_seeds.end(),
                                   [](const auto &a) { return a.empty() || !a.label_columns; }),
                    fwd_seeds.end());
    bwd_seeds.erase(std::remove_if(bwd_seeds.begin(), bwd_seeds.end(),
                                   [](const auto &a) { return a.empty() || !a.label_columns; }),
                    bwd_seeds.end());

    if (terminate() || (fwd_seeds.empty() && bwd_seeds.empty()))
        return { 0, 0 };


    bool has_labels = dynamic_cast<const ILabeledAligner*>(&aligner);

    // filter out empty seeds
    std::vector<Seed> both_seeds[2];
    both_seeds[0].reserve(fwd_seeds.size());
    both_seeds[1].reserve(bwd_seeds.size());
    for (auto&& fwd_seed : fwd_seeds) {
        if (!fwd_seed.empty())
            both_seeds[0].emplace_back(std::move(fwd_seed));
    }
    for (auto&& bwd_seed : bwd_seeds) {
        if (!bwd_seed.empty())
            both_seeds[1].emplace_back(std::move(bwd_seed));
    }

    fwd_seeds = std::vector<Seed>();
    bwd_seeds = std::vector<Seed>();

    // perform chaining on the forward, and the reverse-complement seeds
    ChainDPTable dp_tables[2];
    AlignedVector<int32_t> seed_backtraces[2];

    logger->trace("Chaining forward seeds");
    size_t num_seeds;
    size_t num_nodes;
    std::tie(dp_tables[0], seed_backtraces[0], num_seeds, num_nodes)
        = chain_seeds(config, forward, both_seeds[0]);

    logger->trace("Chaining reverse complement seeds");
    size_t num_seeds_bwd;
    size_t num_nodes_bwd;
    std::tie(dp_tables[1], seed_backtraces[1], num_seeds_bwd, num_nodes_bwd)
        = chain_seeds(config, reverse, both_seeds[1]);

    num_seeds += num_seeds_bwd;
    num_nodes += num_nodes_bwd;

    // construct chains by backtracking
    std::vector<std::tuple<score_t, uint32_t, ssize_t>> starts;
    starts.reserve(dp_tables[0].size() + dp_tables[1].size());
    sdsl::bit_vector both_used[2] {
        sdsl::bit_vector(dp_tables[0].size(), false),
        sdsl::bit_vector(dp_tables[1].size(), false)
    };

    for (size_t i = 0; i < dp_tables[0].size(); ++i) {
        starts.emplace_back(dp_tables[0][i].chain_score, 0, -static_cast<ssize_t>(i));
    }
    for (size_t i = 0; i < dp_tables[1].size(); ++i) {
        starts.emplace_back(dp_tables[1][i].chain_score, 1, -static_cast<ssize_t>(i));
    }

    if (starts.empty()) {
        logger->trace("No chains found");
        return std::make_pair(num_seeds, num_nodes);
    }

    std::sort(starts.begin(), starts.end(), std::greater<decltype(starts)::value_type>());

    score_t last_chain_score = std::numeric_limits<score_t>::min();
    std::unordered_multiset<Chain, ChainHash> chains;

    bool coverage_too_low = false;

    auto flush_chains = [&]() {
        if (coverage_too_low)
            return;

        assert(chains.size());
        auto it = chains.begin();
        Chain last_chain = *it;
        for (++it; it != chains.end(); ++it) {
            const Chain &chain = *it;
            if (chain != last_chain) {
                double exact_match_fraction
                    = static_cast<double>(get_num_char_matches_in_seeds(last_chain.begin(),
                                                                        last_chain.end()))
                        / forward.size();

                if (exact_match_fraction < config.min_exact_match) {
                    coverage_too_low = true;
                    return;
                }

                callback(std::move(last_chain), last_chain_score);
                last_chain = *it;
                continue;
            }

            // if this chain has the same seeds as the last one, merge their coordinate sets
            for (size_t i = 0; i < chain.size(); ++i) {
                Vector<Alignment::Column> columns;
                const auto &last_columns = last_chain[i].first.get_columns();
                const auto &cur_columns = chain[i].first.get_columns();
                if (chain[i].first.label_coordinates.size()) {
                    assert(last_chain[i].first.label_columns
                            == last_chain[i].first.label_coordinates.size());
                    assert(chain[i].first.label_columns
                            == chain[i].first.label_coordinates.size());
                    Alignment::CoordinateSet coord_union;
                    auto add_col_coords = [&](auto col, auto &coords) {
                        columns.push_back(col);
                        coord_union.emplace_back(std::move(coords));
                    };

                    utils::match_indexed_values(
                        last_columns.begin(), last_columns.end(),
                        last_chain[i].first.label_coordinates.begin(),
                        cur_columns.begin(), cur_columns.end(),
                        chain[i].first.label_coordinates.begin(),
                        [&](auto col, const auto &coords, const auto &other_coords) {
                            columns.push_back(col);
                            coord_union.emplace_back();
                            std::set_union(coords.begin(), coords.end(),
                                           other_coords.begin(), other_coords.end(),
                                           std::back_inserter(coord_union.back()));
                        },
                        add_col_coords, add_col_coords
                    );
                    std::swap(last_chain[i].first.label_coordinates, coord_union);
                } else {
                    assert(chain[i].first.label_columns);
                    std::set_union(last_columns.begin(), last_columns.end(),
                                   cur_columns.begin(), cur_columns.end(),
                                   std::back_inserter(columns));
                }
                last_chain[i].first.set_columns(std::move(columns));
            }
        }

        double exact_match_fraction
            = static_cast<double>(get_num_char_matches_in_seeds(last_chain.begin(),
                                                                last_chain.end()))
                / forward.size();

        if (exact_match_fraction < config.min_exact_match) {
            coverage_too_low = true;
            return;
        }

        callback(std::move(last_chain), last_chain_score);

        chains.clear();
    };

    for (const auto &[chain_score, j, neg_i] : starts) {
        if (coverage_too_low || terminate())
            break;

        auto &used = both_used[j];
        uint32_t i = -neg_i;
        if (used[i])
            continue;

        const auto &dp_table = dp_tables[j];
        const auto &seeds = both_seeds[j];
        const auto &seed_backtrace = seed_backtraces[j];

        // iterate through the DP table, adding seeds to the chain
        std::vector<std::pair<Seed, int64_t>> chain_seeds;

        while (i != nid) {
            const auto &[label, coord, clipping, end, score, seed_i] = dp_table[i];
            if (skip_column(label))
                break;

            used[i] = true;
            chain_seeds.emplace_back(seeds[seed_i], coord);
            chain_seeds.back().first.label_encoder = seeds[seed_i].label_encoder;
            if (has_labels) {
                chain_seeds.back().first.set_columns(Vector<Alignment::Column>{ label });
                chain_seeds.back().first.label_coordinates.resize(1);
                chain_seeds.back().first.label_coordinates[0].assign(1, coord);
            }
            i = seed_backtrace[i];
        }

        if (chain_seeds.empty())
            continue;

        // clean chain by merging overlapping seeds
        for (size_t i = chain_seeds.size() - 1; i > 0; --i) {
            auto &cur_seed = chain_seeds[i].first;
            auto &prev_seed = chain_seeds[i - 1].first;

            assert(cur_seed.size());
            assert(prev_seed.size());
            assert(prev_seed.get_clipping() <= cur_seed.get_clipping());
            assert(prev_seed.get_end_clipping() >= cur_seed.get_end_clipping());

            size_t prev_end = prev_seed.get_clipping()
                                + prev_seed.get_query_view().size();
            if (prev_end > cur_seed.get_clipping()) {
                // they overlap
                size_t coord_dist = cur_seed.label_coordinates[0][0]
                                        + cur_seed.get_query_view().size()
                                        - prev_seed.label_coordinates[0][0]
                                        - prev_seed.get_query_view().size();
                size_t dist = cur_seed.get_clipping()
                                + cur_seed.get_query_view().size() - prev_end;

                if (dist == coord_dist && cur_seed.get_nodes().size() >= dist) {
                    prev_seed.expand({ cur_seed.get_nodes().end() - dist,
                                       cur_seed.get_nodes().end() });
                    cur_seed = Seed();
                }
            }
        }

        chain_seeds.erase(std::remove_if(chain_seeds.begin(), chain_seeds.end(),
                                         [](const auto &a) { return a.first.empty(); }),
                          chain_seeds.end());
        assert(chain_seeds.size());

        for (size_t i = chain_seeds.size() - 1; i > 0; --i) {
            assert(chain_seeds[i].first.get_clipping()
                        > chain_seeds[i - 1].first.get_clipping());
            assert(chain_seeds[i].first.get_end_clipping()
                        < chain_seeds[i - 1].first.get_end_clipping());
            chain_seeds[i].second -= chain_seeds[i - 1].second;
            assert(chain_seeds[i].second > 0);
        }

        chain_seeds[0].second = 0;
        if (!chain_seeds[0].first.label_columns)
            continue;

        Chain chain;
        chain.reserve(chain_seeds.size());
        std::transform(chain_seeds.begin(), chain_seeds.end(), std::back_inserter(chain),
                       [&](const auto &c) {
                           return std::make_pair(Alignment(c.first, config), c.second);
                       });

        if (chains.empty()) {
            chains.emplace(std::move(chain));
            last_chain_score = chain_score;
            continue;
        }

        if (chain_score == last_chain_score) {
            chains.emplace(std::move(chain));
            continue;
        }

        flush_chains();
        chains.emplace(std::move(chain));
        last_chain_score = chain_score;
    }

    flush_chains();

    return std::make_pair(num_seeds, num_nodes);
}

std::tuple<ChainDPTable, AlignedVector<int32_t>, size_t, size_t>
chain_seeds(const DBGAlignerConfig &config,
            std::string_view query,
            std::vector<Seed> &seeds) {
    if (seeds.empty())
        return {};

    if (std::any_of(seeds.begin(), seeds.end(),
                    [](const auto &a) { return a.label_coordinates.empty(); })) {
        throw std::runtime_error("Chaining only supported for seeds with coordinates");
    }

    size_t num_nodes = 0;

    ssize_t query_size = query.size();

    ChainDPTable dp_table;
    dp_table.reserve(seeds.size());
    std::reverse(seeds.begin(), seeds.end());

    tsl::hopscotch_map<Alignment::Column, size_t> label_sizes;

    for (size_t i = 0; i < seeds.size(); ++i) {
        const auto &columns = seeds[i].get_columns();
        for (size_t j = 0; j < seeds[i].label_coordinates.size(); ++j) {
            Alignment::Column c = columns[j];
            auto rbegin = seeds[i].label_coordinates[j].rbegin();
            auto rend = rbegin + std::min(seeds[i].label_coordinates[j].size(),
                                          config.max_num_seeds_per_locus);
            std::for_each(rbegin, rend, [&](ssize_t coord) {
                ++label_sizes[c];
                dp_table.emplace_back(c, coord, seeds[i].get_clipping(),
                                      seeds[i].get_clipping() + seeds[i].get_query_view().size(),
                                      seeds[i].get_query_view().size(), i);
            });
        }
        seeds[i].label_columns = 0;
        seeds[i].label_coordinates = Alignment::CoordinateSet{};
    }

    dp_table.reserve(dp_table.size() + 9);

    size_t num_seeds = dp_table.size();
    AlignedVector<int32_t> backtrace(dp_table.size(), nid);
    if (dp_table.empty())
        return std::make_tuple(std::move(dp_table), std::move(backtrace), num_seeds, num_nodes);

    logger->trace("Sorting {} anchors", dp_table.size());
    // sort seeds by label, then by decreasing reference coordinate
    std::sort(dp_table.begin(), dp_table.end(), std::greater<TableElem>());
    logger->trace("Chaining anchors");

    size_t bandwidth = 65;

    // scoring function derived from minimap2
    // https://academic.oup.com/bioinformatics/article/34/18/3094/4994778
    float sl = static_cast<float>(config.min_seed_length) * 0.01;

    size_t cur_label_end = 0;
    size_t i = 0;
    while (cur_label_end < dp_table.size()) {
        cur_label_end += label_sizes[dp_table[i].label];
        for ( ; i < cur_label_end; ++i) {
            const auto &[prev_label, prev_coord, prev_clipping, prev_end,
                         prev_score, prev_seed_i] = dp_table[i];

            if (!prev_clipping)
                continue;

            size_t it_end = std::min(bandwidth, cur_label_end - i) + i;
            ssize_t coord_cutoff = prev_coord - query_size;

            const simde__m256i coord_cutoff_v = simde_mm256_set1_epi64x(coord_cutoff);
            const simde__m256i prev_coord_v = simde_mm256_set1_epi64x(prev_coord);
            const simde__m256i prev_clipping_v = simde_mm256_set1_epi32(prev_clipping);
            const simde__m256i query_size_v = simde_mm256_set1_epi32(query_size);
            const simde__m256i prev_score_v = simde_mm256_set1_epi32(prev_score);
            const simde__m256i it_end_v = simde_mm256_set1_epi32(it_end - 1);
            const simde__m256i i_v = simde_mm256_set1_epi32(i);
            auto epi64_to_epi32 = [](simde__m256i v) {
                return simde_mm256_castsi256_si128(simde_mm256_permute4x64_epi64(simde_mm256_shuffle_epi32(v, 8), 8));
            };

            simde__m256i j_v = simde_mm256_add_epi32(simde_mm256_set1_epi32(i + 1), simde_mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0));
            for (size_t j = i + 1; true; j += 8) {
                // if (coord_cutoff > coord || j > it_end - 1)
                //     break;
                simde__m256i coord_1_v = simde_mm256_i32gather_epi64(&dp_table[j].coordinate, simde_mm_set_epi32(12, 8, 4, 0), 8);
                simde__m256i coord_2_v = simde_mm256_i32gather_epi64(&dp_table[j + 4].coordinate, simde_mm_set_epi32(12, 8, 4, 0), 8);
                simde__m256i coord_1_mask = simde_mm256_cmpgt_epi64(coord_cutoff_v, coord_1_v);
                simde__m256i coord_2_mask = simde_mm256_cmpgt_epi64(coord_cutoff_v, coord_2_v);
                simde__m256i coord_neg_mask = simde_mm256_blend_epi32(coord_1_mask, coord_2_mask, 0b10101010);
                simde__m256i j_neg_mask = simde_mm256_cmpgt_epi32(j_v, it_end_v);
                coord_neg_mask = simde_mm256_or_si256(j_neg_mask, coord_neg_mask);

                // int32_t dist = prev_clipping - clipping;
                simde__m256i clipping_v = simde_mm256_i32gather_epi32(&dp_table[j].seed_clipping, simde_mm256_set_epi32(56, 48, 40, 32, 24, 16, 8, 0), 4);
                simde__m256i dist_v = simde_mm256_sub_epi32(prev_clipping_v, clipping_v);

                // int32_t coord_dist = prev_coord - coord;
                // a[0:32],b[0:32],a[64:96],b[64:96],a[128:160],b[128:160],a[192:224],b[192:224]
                simde__m128i coord_dist_1_v = epi64_to_epi32(simde_mm256_sub_epi64(prev_coord_v, coord_1_v));
                simde__m128i coord_dist_2_v = epi64_to_epi32(simde_mm256_sub_epi64(prev_coord_v, coord_2_v));
                simde__m256i coord_dist_v = simde_mm256_set_m128i(coord_dist_2_v, coord_dist_1_v);

                simde__m256i dist_mask = simde_mm256_cmpgt_epi32(dist_v, simde_mm256_setzero_si256());
                simde__m256i dmax = simde_mm256_max_epi32(dist_v, coord_dist_v);
                simde__m256i dmax_mask = simde_mm256_cmpgt_epi32(query_size_v, dmax);
                dist_mask = simde_mm256_and_si256(dist_mask, dmax_mask);
                dist_mask = simde_mm256_andnot_si256(coord_neg_mask, dist_mask);

                // if (dist > 0 && std::max(dist, coord_dist) < query_size) {
                if (simde_mm256_movemask_epi8(dist_mask)) {
                    // score_t match = std::min({ dist, coord_dist, end - clipping });
                    simde__m256i match_v = simde_mm256_min_epi32(dist_v, coord_dist_v);
                    simde__m256i end_v = simde_mm256_i32gather_epi32(&dp_table[j].seed_end, simde_mm256_set_epi32(56, 48, 40, 32, 24, 16, 8, 0), 4);
                    simde__m256i length_v = simde_mm256_sub_epi32(end_v, clipping_v);
                    match_v = simde_mm256_min_epi32(match_v, length_v);

                    // score_t cur_score = prev_score + match;
                    simde__m256i cur_score_v = simde_mm256_add_epi32(prev_score_v, match_v);

                    // float coord_diff = std::abs(coord_dist - dist);
                    simde__m256i coord_diff = simde_mm256_sub_epi32(coord_dist_v, dist_v);
                    coord_diff = simde_mm256_abs_epi32(coord_diff);
                    simde__m256 coord_diff_f = simde_mm256_cvtepi32_ps(coord_diff);

                    // float linear_penalty = coord_diff * sl;
                    simde__m256 linear_penalty_v = simde_mm256_mul_ps(coord_diff_f, simde_mm256_set1_ps(sl));

                    // float log_penalty = log2(coord_diff + 1) * 0.5;
                    simde__m256 log_penalty_v = simde_mm256_log2_ps(simde_mm256_cvtepi32_ps(simde_mm256_add_epi32(coord_diff, simde_mm256_set1_epi32(1))));
                    log_penalty_v = simde_mm256_mul_ps(log_penalty_v, simde_mm256_set1_ps(0.5));

                    // cur_score -= linear_penalty + log_penalty;
                    simde__m256 gap_penalty_f = simde_mm256_add_ps(linear_penalty_v, log_penalty_v);
                    simde__m256i gap_penalty_v = simde_mm256_cvtps_epi32(gap_penalty_f);
                    simde__m256i dist_cutoff_mask = simde_mm256_cmpgt_epi32(coord_diff, simde_mm256_setzero_si256());
                    gap_penalty_v = simde_mm256_blendv_epi8(simde_mm256_setzero_si256(), gap_penalty_v, dist_cutoff_mask);
                    cur_score_v = simde_mm256_sub_epi32(cur_score_v, gap_penalty_v);

                    // if (cur_score >= score) {
                    //     score = cur_score;
                    //     backtrace[j] = i;
                    // }
                    simde__m256i old_scores_v = simde_mm256_i32gather_epi32(&dp_table[j].chain_score, simde_mm256_set_epi32(56, 48, 40, 32, 24, 16, 8, 0), 4);
                    simde__m256i score_neg_mask = simde_mm256_cmpgt_epi32(old_scores_v, cur_score_v);
                    simde__m256i mask = simde_mm256_andnot_si256(score_neg_mask, dist_mask);

                    cur_score_v = simde_mm256_blendv_epi8(old_scores_v, cur_score_v, mask);
                    simde_mm256_maskstore_epi32(&backtrace[j], mask, i_v);

                    // TODO: simde_mm256_i32scatter_epi32 not implemented yet
                    score_t cur_scores[8] SIMDE_ALIGN_TO_32;
                    simde_mm256_store_si256((simde__m256i*)cur_scores, cur_score_v);
                    auto *dp_table_o = &dp_table[j];
                    dp_table_o[0].chain_score = cur_scores[0];
                    dp_table_o[1].chain_score = cur_scores[1];
                    dp_table_o[2].chain_score = cur_scores[2];
                    dp_table_o[3].chain_score = cur_scores[3];
                    dp_table_o[4].chain_score = cur_scores[4];
                    dp_table_o[5].chain_score = cur_scores[5];
                    dp_table_o[6].chain_score = cur_scores[6];
                    dp_table_o[7].chain_score = cur_scores[7];
                }

                if (simde_mm256_movemask_epi8(coord_neg_mask))
                    break;

                j_v = simde_mm256_add_epi32(j_v, simde_mm256_set1_epi32(8));
            }
#if 0
            // reference implementation
            for (size_t j = i + 1; j < it_end; ++j) {
                auto &[label, coord, clipping, end, score, seed_i] = dp_table[j];
                assert(label == prev_label);

                if (coord_cutoff > coord)
                    break;

                int32_t dist = prev_clipping - clipping;
                int32_t coord_dist = prev_coord - coord;
                if (dist > 0 && std::max(dist, coord_dist) < query_size) {
                    score_t match = std::min({ dist, coord_dist, end - clipping });
                    score_t cur_score = prev_score + match;
                    if (coord_dist != dist) {
                        float coord_diff = std::abs(coord_dist - dist);
                        float linear_penalty = coord_diff * sl;
                        float log_penalty = log2(coord_diff + 1) * 0.5;
                        cur_score -= linear_penalty + log_penalty;
                    }
                    if (cur_score >= score) {
                        score = cur_score;
                        backtrace[j] = i;
                    }
                }
            }
#endif
        }
    }

    return std::make_tuple(std::move(dp_table), std::move(backtrace), num_seeds, num_nodes);
}

std::tuple<size_t, size_t, size_t>
chain_and_filter_seeds(const IDBGAligner &aligner,
                       std::shared_ptr<ISeeder> &seeder,
                       SeedFilteringExtender&& extender) {
    std::string_view query = extender.get_query();
    const DeBruijnGraph &graph_ = aligner.get_graph();
    const DBGAlignerConfig &config_ = aligner.get_config();
    auto path_index = graph_.get_extension_threadsafe<IPathIndex>();
    const auto *canonical = dynamic_cast<const CanonicalDBG*>(&graph_);
    const auto &anchors = seeder->get_alignments();
    if (anchors.empty())
        return {};

    size_t num_seeds = 0;
    size_t num_extensions = 0;
    size_t num_explored_nodes = 0;

    bool orientation = anchors[0].get_orientation();

    num_seeds += anchors.size();
    std::vector<node_index> nodes;
    nodes.reserve(anchors.size());
    std::vector<std::pair<size_t, size_t>> anchor_ids;
    anchor_ids.reserve(nodes.size());

    std::vector<std::vector<node_index>> out_nodes;
    out_nodes.reserve(anchors.size());

    for (size_t i = 0; i < anchors.size(); ++i) {
        const auto &anchor = anchors[i];
        for (size_t j = 0; j < anchor.get_nodes().size(); ++j) {
            node_index node = anchor.get_nodes()[j];
            auto &out = out_nodes.emplace_back();
            graph_.adjacent_outgoing_nodes(node, [&](node_index next) {
                out.emplace_back(next);
            });

            if (canonical) {
                nodes.emplace_back(canonical->get_base_node(node));
            } else {
                nodes.emplace_back(node);
            }
            anchor_ids.emplace_back(i, j);
        }
    }

    using Anchor = std::tuple<std::string_view::const_iterator, // end
                              Alignment::Column, // annotation
                              int64_t, // dummy
                              std::string_view::const_iterator, // begin
                              uint64_t, // mem_length
                              score_t, // score
                              uint32_t, // last
                              int64_t, // last_dist
                              uint64_t // index
    >;
    static_assert(sizeof(Anchor) == 64);

    auto node_coords = path_index->get_coords(nodes);
    size_t j = 0;
    std::vector<Anchor> seeds;
    for (const auto &anchor : anchors) {
        size_t offset = anchor.get_offset();
        auto begin = anchor.get_query_view().begin();
        auto end = anchor.get_query_view().begin() + graph_.get_k() - offset;
        for (size_t k = 0; k < anchor.get_nodes().size(); ++k) {
            auto add_seed = [&](Alignment::Column col) {
                assert(anchor.get_nodes()[k] == nodes[j]
                    || (canonical && nodes[j] == canonical->get_base_node(anchor.get_nodes()[k])));
                seeds.emplace_back(end,
                                   col,
                                   0,
                                   begin,
                                   end - begin,
                                   end - begin,
                                   std::numeric_limits<uint32_t>::max(),
                                   anchor.get_clipping(),
                                   j);
            };

            if (anchor.label_columns) {
                for (auto col : anchor.get_columns()) {
                    add_seed(col);
                }
            } else {
                add_seed(std::numeric_limits<Alignment::Column>::max());
            }
            ++j;
            ++end;
            if (offset) {
                --offset;
            } else {
                ++begin;
            }
        }
    }

    const auto *labeled_aligner = dynamic_cast<const ILabeledAligner*>(&aligner);

    std::sort(seeds.begin(), seeds.end());
    assert(std::adjacent_find(seeds.begin(), seeds.end()) == seeds.end());
    size_t bandwidth = 65;
    ssize_t min_overlap = config_.min_seed_length;
    float sl = -static_cast<float>(config_.min_seed_length) * 0.01;
    score_t match_score = config_.match_score("A");
    score_t gap_open = config_.gap_opening_penalty / match_score;
    score_t gap_ext = config_.gap_extension_penalty / match_score;
    score_t node_insert = config_.node_insertion_penalty / match_score;

    for (size_t j = 0; j < seeds.size() - 1; ++j) {
        const auto &[end_j, col_j, dummy_j, begin_j, aln_length_j, score_j, last_j, last_dist_j, coord_idx_j] = seeds[j];
        const auto &coords_j = node_coords[coord_idx_j];
        bool is_rev_j = (nodes[coord_idx_j] != anchors[anchor_ids[coord_idx_j].first].get_nodes()[anchor_ids[coord_idx_j].second]);
        size_t num_considered = 0;
        bool updated_with_less = false;
        size_t j_update = std::numeric_limits<size_t>::max();
        size_t k = j;
        if (config_.allow_jump) {
            while (k && std::get<0>(seeds[k]) >= end_j) {
                --k;
            }
        }

        for (++k; k < seeds.size(); ++k) {
            auto &[end, col, dummy, begin, aln_length, score, last, last_dist, coord_idx] = seeds[k];
            assert(end >= end_j);
            if (k == j)
                continue;

            if (!labeled_aligner && col != col_j)
                continue;

            if (++num_considered > bandwidth)
                break;

            int64_t dist = end - end_j;

            score_t label_change_score = labeled_aligner
                ? labeled_aligner->get_label_change_score(col_j, col)
                : 0;

            size_t num_added = end - std::max(begin, end_j);
            score_t base_added_score = score_j + num_added + label_change_score;
            if (base_added_score < score)
                continue;

            size_t length = end - begin;

            const auto &coords = node_coords[coord_idx];
            auto it = coords_j.begin();
            auto jt = coords.begin();
            bool updated = false;

            if (config_.allow_jump && aln_length_j >= graph_.get_k()) {
                if (end == end_j && end_j - begin >= min_overlap) {
                    // same suffix seeds matching to different nodes
                    score_t updated_score = base_added_score + node_insert;
                    if (updated_score > score) {
                        score = updated_score;
                        last_dist = 0;
                        last = j;
                        updated = true;
                        aln_length = length;
                    }
                }

                if (begin >= end_j) {
                    // perhaps a disjoint alignment?
                    score_t gap = begin - end_j;
                    score_t gap_cost = node_insert + gap_open + gap_open
                                        + std::max(gap - 1, 0) * gap_ext;
                    score_t updated_score = base_added_score + gap_cost;

                    if (updated_score > score) {
                        score = updated_score;
                        last_dist = std::numeric_limits<uint32_t>::max() + dist;
                        last = j;
                        updated = true;
                        aln_length = length;
                    }
                }
            }

            if (dist > 0) {
                node_index node = anchors[anchor_ids[coord_idx].first].get_nodes()[anchor_ids[coord_idx].second];
                int64_t coord_dist = 1;
                float gap = dist - coord_dist;
                bool found = false;
                score_t gap_cost = ceil(sl * gap - log2(gap + 1) * 0.5);
                assert(gap > 0 || gap_cost == 0);
                score_t updated_score = base_added_score + gap_cost;
                for (node_index next : out_nodes[coord_idx_j]) {
                    if (next == node) {
                        if (std::tie(updated_score, last_dist) > std::tie(score, coord_dist)) {
                            found = true;
                            score = updated_score;
                            last_dist = coord_dist;
                            last = j;
                            updated = true;
                            aln_length = aln_length_j + coord_dist;
                        }
                        break;
                    }
                }

                if (found && gap == 0)
                    continue;

                bool is_rev = (nodes[coord_idx] != node);
                if (is_rev != is_rev_j)
                    continue;

                while (it != coords_j.end() && jt != coords.end()) {
                    if (it->first == jt->first) {
                        for (int64_t c_j : it->second) {
                            for (int64_t c : jt->second) {
                                int64_t coord_dist = c - c_j;
                                if (is_rev)
                                    coord_dist = -coord_dist;

                                if (coord_dist <= 0)
                                    continue;

                                float gap = std::abs(coord_dist - dist);
                                score_t gap_cost = ceil(sl * gap - log2(gap + 1) * 0.5);
                                assert(gap > 0 || gap_cost == 0);
                                score_t updated_score = base_added_score + gap_cost;

                                if (std::tie(updated_score, last_dist) > std::tie(score, coord_dist)) {
                                    score = updated_score;
                                    last_dist = coord_dist;
                                    last = j;
                                    updated = true;
                                    aln_length = aln_length_j + coord_dist;
                                }
                            }
                        }
                        ++it;
                        ++jt;
                    } else if (it->first < jt->first) {
                        ++it;
                    } else {
                        ++jt;
                    }
                }
            }

            if (updated && k < last) {
                updated_with_less = true;
                j_update = std::min(j_update, k - 1);
            }
        }

        if (updated_with_less)
            j = j_update;
    }

    tsl::hopscotch_map<Alignment::Column, size_t> used_cols;
    sdsl::bit_vector used(seeds.size(), false);
    std::vector<std::tuple<score_t, size_t, size_t>> best_chain;
    best_chain.reserve(seeds.size());
    for (size_t j = 0; j < seeds.size(); ++j) {
        const auto &[end, col, last_q_dist, begin, mem_length, score, last, last_dist, coord_idx] = seeds[j];
        best_chain.emplace_back(-score, last_dist, j);
    }

    std::sort(best_chain.begin(), best_chain.end());

    sdsl::bit_vector matching_pos(query.size(), false);
    std::vector<Alignment> alignments;
    for (size_t j = 0; j < best_chain.size(); ++j) {
        auto [nscore, last_dist, k] = best_chain[j];
        if (used[k])
            continue;

        Alignment::Column col = std::get<1>(seeds[k]);
        if (++used_cols[col] > config_.num_alternative_paths)
            continue;

        logger->trace("Chain\t{}", -nscore);

        std::vector<std::pair<Seed, size_t>> seed_chain;
        while (k != std::numeric_limits<uint32_t>::max()) {
            const auto &[end, col, last_q_dist, begin, mem_length, score, last, last_dist, coord_idx] = seeds[k];
            auto [anchor_i, anchor_node_i] = anchor_ids[coord_idx];
            const auto &anchor = anchors[anchor_i];
            used[k] = true;
            auto &seed = seed_chain.emplace_back(
                Seed(std::string_view(begin, end - begin),
                     std::vector<node_index>{ anchor.get_nodes()[anchor_node_i] },
                     orientation,
                     graph_.get_k() - (end - begin),
                     begin - query.begin(),
                     query.end() - end),
                last_dist).first;
            seed.label_encoder = anchor.label_encoder;
            if (seed.label_encoder)
                seed.label_columns = seed.label_encoder->cache_column_set(1, col);

            k = last;
        }

        for (auto it = seed_chain.rbegin() + 1; it != seed_chain.rend(); ++it) {
            it->second += (it - 1)->second;
        }

        // merge seeds
        for (auto jt = seed_chain.begin(); jt + 1 != seed_chain.end(); ++jt) {
            if (jt->first.get_clipping() - (jt + 1)->first.get_clipping() == 1
                    && jt->second - (jt + 1)->second == 1
                    && jt->first.label_columns == (jt + 1)->first.label_columns) {
                assert(graph_.traverse((jt + 1)->first.get_nodes().back(),
                                       *(jt + 1)->first.get_query_view().end())
                        == jt->first.get_nodes()[0]);
                (jt + 1)->first.expand(jt->first.get_nodes());
                jt->first = Seed();
            }
        }

        auto jt = std::make_reverse_iterator(
            std::remove_if(seed_chain.begin(), seed_chain.end(),
                           [](const auto &a) { return a.first.empty(); })
        );

        Chain chain;
        for (auto it = jt; it != seed_chain.rend(); ++it) {
            chain.emplace_back(Alignment(it->first, config_),
                               it != jt
                                   ? it->second + it->first.get_query_view().size()
                                        - (it - 1)->second - (it - 1)->first.get_query_view().size()
                                   : 0);
            logger->trace("\t{}\t(dist: {}{})", chain.back().first,
                          it != jt && (it - 1)->first.get_query_view().end()
                                         == it->first.get_query_view().begin() + graph_.get_k() - it->first.get_offset()
                              ? "0 + " : "",
                          chain.back().second);
        }

        aligner.extend_chain(std::move(chain), extender, [&](Alignment&& aln) {
            logger->trace("\tChaining produced alignment\t{}", aln);
            aln.get_cigar().mark_exact_matches(matching_pos);
            alignments.emplace_back(std::move(aln));
        }, true);
    }

    num_extensions += extender.num_extensions();
    num_explored_nodes += extender.num_explored_nodes();
    seeder = std::make_unique<ManualSeeder>(std::move(alignments),
                                            sdsl::util::cnt_one_bits(matching_pos));

    return std::make_tuple(num_seeds, num_extensions, num_explored_nodes);
}

} // namespace align
} // namespace graph
} // namespace mtg
