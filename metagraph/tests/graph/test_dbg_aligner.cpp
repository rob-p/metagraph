#include <gtest/gtest.h>

#define private public
#include "dbg_aligner.hpp"
#include "aligner_methods.hpp"

#include "boss.hpp"
#include "dbg_succinct.hpp"
#include "dbg_hash_string.hpp"
#include "dbg_hash_ordered.hpp"
#include "annotate_column_compressed.hpp"
#include "reverse_complement.hpp"
#include "../test_helpers.hpp"
#include "test_dbg_helpers.hpp"


typedef DBGAligner::score_t score_t;

const DBGAlignerConfig config;

int8_t single_char_score(const DBGAlignerConfig &config, char a, int8_t b) {
    return config.get_row(a)[b];
}

bool check_extend(std::shared_ptr<const DeBruijnGraph> graph,
                  const DBGAlignerConfig &config,
                  const std::vector<DBGAligner::DBGAlignment> &paths,
                  const std::string &query,
                  score_t min_path_score = std::numeric_limits<score_t>::min()) {
    assert(graph.get());

    std::vector<DeBruijnGraph::node_index> nodes;
    graph->map_to_nodes_sequentially(query.begin(),
                                     query.end(),
                                     [&](auto node) { nodes.emplace_back(node); });

    auto ext_paths = DBGAligner(*graph,
                                config,
                                make_unimem_seeder(nodes)).align(
        query,
        false,
        min_path_score
    );

    return std::equal(paths.begin(), paths.end(),
                      ext_paths.begin(), ext_paths.end());
}

template <typename Graph>
class DBGAlignerTest : public DeBruijnGraphTest<Graph> {};

TYPED_TEST_CASE(DBGAlignerTest, GraphTypes);

TYPED_TEST(DBGAlignerTest, align_sequence_too_short) {
    size_t k = 4;
    std::string reference = "CATTT";
    std::string query =     "CAT";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);

    EXPECT_EQ(0ull, alt_paths.size());
}

TYPED_TEST(DBGAlignerTest, align_single_node) {
    size_t k = 3;
    std::string reference = "CAT";
    std::string query =     "CAT";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(1ull, path.size());
    EXPECT_EQ("CAT", path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end()),
              path.get_score());
    EXPECT_EQ("3=", path.get_cigar().to_string());
    EXPECT_EQ(3u, path.get_num_matches());
    EXPECT_TRUE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, align_iterators_straight) {
    size_t k = 4;
    std::string reference = "AGCTTCGAGGCCAA";
    std::string query = reference;

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    std::vector<DBGAligner::DBGAlignment> alt_paths;
    aligner.align(query.begin(), query.end(),
                  [&](auto&& alignment) { alt_paths.emplace_back(std::move(alignment)); });
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(query, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end()),
              path.get_score());
    EXPECT_EQ("14=", path.get_cigar().to_string());
    EXPECT_EQ(14u, path.get_num_matches());
    EXPECT_TRUE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, align_straight) {
    size_t k = 4;
    std::string reference = "AGCTTCGAGGCCAA";
    std::string query = reference;

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto paths = aligner.align(query);
    ASSERT_FALSE(paths.empty());

    EXPECT_EQ(1ull, paths.size());
    auto path = paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(query, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end()),
              path.get_score());
    EXPECT_EQ("14=", path.get_cigar().to_string());
    EXPECT_EQ(14u, path.get_num_matches());
    EXPECT_TRUE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
}

TYPED_TEST(DBGAlignerTest, align_straight_forward_and_reverse_complement) {
    size_t k = 4;
    std::string reference = "AGCTTCGAGGCCAA";
    std::string query = reference;
    reverse_complement(query.begin(), query.end());

    auto graph = build_graph_batch<TypeParam>(k, { reference });

    DBGAligner aligner(*graph);
    auto paths = aligner.align_forward_and_reverse_complement(query, reference);
    ASSERT_FALSE(paths.empty());

    EXPECT_EQ(1ull, paths.size());
    auto path = paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(reference, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end()),
              path.get_score());
    EXPECT_EQ("14=", path.get_cigar().to_string());
    EXPECT_EQ(14u, path.get_num_matches());
    EXPECT_TRUE(path.is_exact_match());

    auto ext_paths = extend_mapping_forward_and_reverse_complement(query, reference, *graph);

    EXPECT_TRUE(std::equal(paths.begin(), paths.end(),
                           ext_paths.begin(), ext_paths.end()));
}


TYPED_TEST(DBGAlignerTest, align_ending_branch) {
    size_t k = 4;
    std::string reference_1 = "AGCTTCGAA";
    std::string reference_2 = "AGCTTCGAC";
    std::string query = reference_2;

    auto graph = build_graph_batch<TypeParam>(k, { reference_1, reference_2 });
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(query, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end()),
              path.get_score());
    EXPECT_EQ("9=", path.get_cigar().to_string());
    EXPECT_EQ(9u, path.get_num_matches());
    EXPECT_TRUE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, align_branch) {
    size_t k = 4;
    std::string reference_1 = "AGCTTCGA" "AT" "ATTTGTT";
    std::string reference_2 = "AGCTTCGA" "CG" "ATTTGTT";
    std::string query = reference_2;

    auto graph = build_graph_batch<TypeParam>(k, { reference_1, reference_2 });
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(query, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end()),
              path.get_score());
    EXPECT_EQ("17=", path.get_cigar().to_string());
    EXPECT_EQ(17u, path.get_num_matches());
    EXPECT_TRUE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, repetitive_sequence_alignment) {
    size_t k = 3;
    std::string reference = "AGGGGGGGGGAAAAGGGGGGG";
    std::string query =     "AGGGGG";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(query, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end()),
              path.get_score());
    EXPECT_EQ("6=", path.get_cigar().to_string());
    EXPECT_EQ(6u, path.get_num_matches());
    EXPECT_TRUE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, variation) {
    size_t k = 4;
    std::string reference = "AGCAA" "C" "TCGAAA";
    std::string query =     "AGCAA" "T" "TCGAAA";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(reference, path.get_sequence());
    EXPECT_EQ(config.score_sequences(query.begin(), query.end(),
                                     reference.begin(), reference.end()),
              path.get_score());
    EXPECT_EQ("5=1X6=", path.get_cigar().to_string());
    EXPECT_EQ(11u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, variation_in_branching_point) {
    size_t k = 4;
    std::string reference_1 = "TTAAGCAA" "CTC" "GAAA";
    std::string reference_2 = "TTAAGCAA" "GTC" "GAAA";
    std::string query =       "TTAAGCAA" "TGG" "GAAA";

    auto graph = build_graph_batch<TypeParam>(k, { reference_1, reference_2 });

    DBGAlignerConfig config;
    config.gap_opening_penalty = -3;
    config.gap_extension_penalty = -1;
    config.set_mismatch_transition_score(-1);
    config.set_mismatch_transversion_score(-2);
    config.set_match_score(2);
    DBGAligner aligner(*graph, config);

    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_TRUE(path.get_sequence().compare(reference_1) == 0 ||
                path.get_sequence().compare(reference_2) == 0)
        << "Path: " << path.get_sequence() << std::endl
        << "Ref1: " << reference_1 << std::endl
        << "Ref2: " << reference_2 << std::endl;
    // TODO: what about other cases?
    EXPECT_EQ("8=3X4=", path.get_cigar().to_string());
    EXPECT_EQ(12u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, multiple_variations) {
    size_t k = 4;
    std::string reference = "ACGCAA" "C" "TCTCTG" "A" "A" "C" "TTGT";
    std::string query =     "ACGCAA" "T" "TCTCTG" "T" "A" "T" "TTGT";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(reference, path.get_sequence());
    EXPECT_EQ(config.score_sequences(query.begin(), query.end(),
                                     reference.begin(), reference.end()),
              path.get_score());
    EXPECT_EQ("6=1X6=1X1=1X4=", path.get_cigar().to_string());
    EXPECT_EQ(17u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, noise_in_branching_point) {
    size_t k = 4;
    std::string reference_1 = "AAAA" "CTTTTTT";
    std::string reference_2 = "AAAA" "TTGGGGG";
    std::string query =       "AAAA" "TTTTTTT";

    auto graph = build_graph_batch<TypeParam>(k, { reference_1, reference_2 });

    DBGAlignerConfig config;
    config.num_alternative_paths = 2;
    config.gap_opening_penalty = -3;
    config.gap_extension_penalty = -1;
    config.set_mismatch_transition_score(-1);
    config.set_mismatch_transversion_score(-2);
    config.set_match_score(2);
    DBGAligner aligner(*graph, config);

    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(2u, alt_paths.size());
    EXPECT_NE(alt_paths.front(), alt_paths.back());
    auto path = alt_paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(reference_1, path.get_sequence());
    EXPECT_EQ(config.score_sequences(query.begin(), query.end(),
                                     reference_1.begin(), reference_1.end()),
              path.get_score());
    EXPECT_EQ("4=1X6=", path.get_cigar().to_string());
    EXPECT_EQ(10u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TYPED_TEST(DBGAlignerTest, alternative_path_basic) {
    size_t k = 4;
    std::vector<std::string> references = {"ACAA" "TTTT" "TTTT",
                                           "ACAA" "TTTT" "TGTT",
                                           "ACAA" "GTTT" "TTTT",
                                           "ACAA" "GTTT" "TGTT"};
    std::string query =                    "ACAA" "CTTT" "TCTT";

    auto graph = build_graph_batch<TypeParam>(k, references);

    DBGAlignerConfig config;
    config.num_alternative_paths = 2;
    config.queue_size = 100;
    config.gap_opening_penalty = -3;
    config.gap_extension_penalty = -1;
    config.set_mismatch_transition_score(-1);
    config.set_mismatch_transversion_score(-2);
    config.set_match_score(2);
    DBGAligner aligner(*graph, config);

    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(config.num_alternative_paths, alt_paths.size());
    for (const auto &path : alt_paths) {
        EXPECT_EQ("4=1X4=1X2=", path.get_cigar().to_string())
            << query << "\n" << path.get_sequence();
        EXPECT_EQ(10u, path.get_num_matches());
        EXPECT_FALSE(path.is_exact_match());
    }

    // TODO check with extend_mapping
}

TYPED_TEST(DBGAlignerTest, align_multiple_misalignment) {
    size_t k = 4;
    std::string reference = "AAAG" "C" "GGACCCTTT" "C" "CGTTAT";
    std::string query =     "AAAG" "G" "GGACCCTTT" "T" "CGTTAT";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto paths = aligner.align(query);
    ASSERT_FALSE(paths.empty());

    EXPECT_EQ(1ull, paths.size());
    auto path = paths.front();

    EXPECT_EQ(query.size() - k + 1, path.size());
    EXPECT_EQ(reference, path.get_sequence());
    EXPECT_EQ(config.score_sequences(query.begin(), query.end(),
                                     reference.begin(), reference.end()),
              path.get_score());
    EXPECT_EQ("4=1X9=1X6=", path.get_cigar().to_string());
    EXPECT_EQ(19u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
}

TYPED_TEST(DBGAlignerTest, align_insert_non_existent) {
    size_t k = 4;
    std::string reference = "TTTCC"     "TTGTT";
    std::string query =     "TTTCC" "A" "TTGTT";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto paths = aligner.align(query);
    ASSERT_FALSE(paths.empty());

    EXPECT_EQ(1ull, paths.size());
    auto path = paths.front();

    EXPECT_EQ(reference.size() - k + 1, path.size());
    EXPECT_EQ(reference, path.get_sequence());
    EXPECT_EQ(config.match_score(reference.begin(), reference.end())
                + config.gap_opening_penalty,
              path.get_score());
    EXPECT_EQ("5=1I5=", path.get_cigar().to_string());
    EXPECT_EQ(10u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
}

TYPED_TEST(DBGAlignerTest, align_delete) {
    size_t k = 4;
    std::string reference = "TTCGA" "T" "TGGCCT";
    std::string query =     "TTCGA"     "TGGCCT";
    // alt query            "TTCGA" "T"  "GGCCT"

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto paths = aligner.align(query);
    ASSERT_FALSE(paths.empty());

    ASSERT_EQ(1ull, paths.size());
    auto path = paths.front();
    EXPECT_FALSE(path.is_exact_match());
    EXPECT_EQ(reference.size() - k + 1, path.size());
    EXPECT_EQ(reference, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end())
                + config.gap_opening_penalty,
              path.get_score());

    std::unordered_set<std::string> possible_cigars { "6=1D5=", "5=1D6=" };

    EXPECT_NE(possible_cigars.end(), possible_cigars.find(path.get_cigar().to_string()));
    EXPECT_FALSE(path.is_exact_match());

    Cigar cigar1, cigar2;
    cigar1.append(Cigar::Operator::MATCH, 6);
    cigar1.append(Cigar::Operator::DELETION, 1);
    cigar1.append(Cigar::Operator::MATCH, 5);

    cigar2.append(Cigar::Operator::MATCH, 5);
    cigar2.append(Cigar::Operator::DELETION, 1);
    cigar2.append(Cigar::Operator::MATCH, 6);

    path.set_cigar(std::move(cigar1));
    auto alt_path = path;
    alt_path.set_cigar(std::move(cigar2));

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), { path }, query)
        || check_extend(graph, aligner.get_config(), { alt_path }, query));
}

TYPED_TEST(DBGAlignerTest, align_gap) {
    size_t k = 4;
    std::string reference = "TTTCTGTATA" "CCTT" "GGCGCTCTC";
    std::string query =     "TTTCTGTATA"        "GGCGCTCTC";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAligner aligner(*graph);
    auto paths = aligner.align(query);
    ASSERT_FALSE(paths.empty());

    EXPECT_EQ(1ull, paths.size());
    auto path = paths.front();

    EXPECT_EQ(reference.size() - k + 1, path.size());
    EXPECT_EQ(reference, path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin(), query.end())
                + config.gap_opening_penalty
                + score_t(3) * config.gap_extension_penalty,
              path.get_score());
    EXPECT_EQ("10=4D9=", path.get_cigar().to_string());
    EXPECT_EQ(19u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
}

TEST(DBGAlignerTest, align_inexact_seeding1) {
    size_t k = 4;
    std::string reference = "GGCC" "TGTTTG";
    std::string query =     "ACCC" "TGTTTG";

    auto graph = std::make_shared<DBGSuccinct>(k);
    graph->add_sequence(reference);
    DBGAligner aligner(*graph);
    auto alt_paths = aligner.align(query);
    ASSERT_FALSE(alt_paths.empty());

    EXPECT_EQ(1ull, alt_paths.size());
    auto path = alt_paths.front();

    EXPECT_EQ(5ull, path.size());
    EXPECT_EQ(reference.substr(2), path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin() + 2, query.end()),
              path.get_score());
    EXPECT_EQ("2S8=", path.get_cigar().to_string())
        << reference.substr(2) << " " << path.get_sequence();
    EXPECT_EQ(8u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), alt_paths, query));
}

TEST(DBGAlignerTest, align_inexact_seeding2) {
    size_t k = 4;
    std::string reference = "AAA" "AGCTTCGAGGCCAA";
    std::string query =      "TT" "AGCTTCGAGGCCAA";

    auto graph = std::make_shared<DBGSuccinct>(k);
    graph->add_sequence(reference);
    DBGAligner aligner(*graph);
    auto paths = aligner.align(query);
    ASSERT_FALSE(paths.empty());

    EXPECT_EQ(1ull, paths.size());
    auto path = paths.front();

    EXPECT_EQ(11u, path.size());
    EXPECT_EQ(reference.substr(3), path.get_sequence());
    EXPECT_EQ(config.match_score(query.begin() + 2, query.end()),
              path.get_score());
    EXPECT_EQ("2S14=", path.get_cigar().to_string());
    EXPECT_EQ(14u, path.get_num_matches());
    EXPECT_FALSE(path.is_exact_match());

    EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
}

TEST(DBGAlignerTest, align_inexact_seed_snp) {
    size_t k = 7;
    std::string reference = "AAAAG" "CTTTCGAGGCCAA";
    std::string query =        "AC" "CTTTCGAGGCCAA";

    auto graph = std::make_shared<DBGSuccinct>(k);
    graph->add_sequence(reference);

    {
        DBGAlignerConfig config;
        config.min_cell_score = 0;
        DBGAligner aligner(*graph, config);

        auto paths = aligner.align(query);
        ASSERT_EQ(1ull, paths.size());
        auto path = paths.front();

        EXPECT_EQ(7u, path.size());
        EXPECT_EQ(reference.substr(5), path.get_sequence());
        EXPECT_EQ(config.match_score(query.begin() + 2, query.end()),
                  path.get_score());
        EXPECT_EQ("2S13=", path.get_cigar().to_string());
        EXPECT_EQ(13u, path.get_num_matches());
        EXPECT_FALSE(path.is_exact_match());

        EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
    }
    {
        DBGAlignerConfig config;
        config.min_seed_length = 2;
        config.min_cell_score = std::numeric_limits<score_t>::min();
        DBGAligner aligner(*graph, config);
        auto paths = aligner.align(query, false, std::numeric_limits<score_t>::min());
        ASSERT_EQ(1ull, paths.size());
        auto path = paths.front();

        EXPECT_EQ(7u, path.size());
        EXPECT_EQ(reference.substr(5), path.get_sequence());
        EXPECT_EQ(config.match_score(query.begin() + 2, query.end()),
                  path.get_score());
        EXPECT_EQ("2S13=", path.get_cigar().to_string());
        EXPECT_EQ(13u, path.get_num_matches());
        EXPECT_FALSE(path.is_exact_match());

        EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
    }
    {
        DBGAlignerConfig config;
        config.min_cell_score = std::numeric_limits<score_t>::min();
        DBGAligner aligner(*graph, config);
        auto paths = aligner.align(query, false, std::numeric_limits<score_t>::min());
        ASSERT_EQ(1ull, paths.size());
        auto path = paths.front();

        EXPECT_EQ(7u, path.size());
        EXPECT_EQ(reference.substr(5), path.get_sequence());
        EXPECT_EQ(config.match_score(query.begin() + 2, query.end()),
                  path.get_score());
        EXPECT_EQ("2S13=", path.get_cigar().to_string());
        EXPECT_EQ(13u, path.get_num_matches());
        EXPECT_FALSE(path.is_exact_match());

        EXPECT_TRUE(check_extend(graph, aligner.get_config(), paths, query));
    }
    {
        DBGAlignerConfig config;
        config.max_num_seeds_per_locus = std::numeric_limits<size_t>::max();
        config.min_cell_score = std::numeric_limits<score_t>::min();
        DBGAligner aligner(*graph, config);
        auto paths = aligner.align(query, false, std::numeric_limits<score_t>::min());
        ASSERT_EQ(1ull, paths.size());
        auto path = paths.front();

        EXPECT_EQ(15u, path.size()); // includes dummy k-mers
        EXPECT_EQ(reference.substr(3), path.get_sequence());
        EXPECT_EQ(config.score_sequences(query.begin(), query.end(),
                                         reference.begin() + 3, reference.end()),
                  path.get_score());
        EXPECT_EQ("1=1X13=", path.get_cigar().to_string());
        EXPECT_EQ(14u, path.get_num_matches());
        EXPECT_FALSE(path.is_exact_match());

        // the unimem alignment mode skips partial k-mer matches in the beginning
        EXPECT_FALSE(check_extend(graph, aligner.get_config(), paths, query));
    }
}
