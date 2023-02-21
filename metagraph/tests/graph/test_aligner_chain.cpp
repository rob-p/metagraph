#include <gtest/gtest.h>

#include "all/test_dbg_helpers.hpp"
#include "test_aligner_helpers.hpp"

#include "graph/alignment/dbg_aligner.hpp"


namespace {

using namespace mtg;
using namespace mtg::graph;
using namespace mtg::graph::align;
using namespace mtg::test;
using namespace mtg::kmer;

template <typename Graph>
class DBGAlignerPostChainTest : public DeBruijnGraphTest<Graph> {};

TYPED_TEST_SUITE(DBGAlignerPostChainTest, FewGraphTypes);

inline void check_chain(const AlignmentResults &paths,
                        const DeBruijnGraph &graph,
                        const DBGAlignerConfig &config,
                        bool has_chain = true) {
    for (const auto &path : paths) {
        EXPECT_TRUE(path.is_valid(graph, &config)) << path;
        if (has_chain) {
            EXPECT_THROW(path.to_json(graph.get_k(), false, "", ""), std::runtime_error);
        } else {
            check_json_dump_load(graph, path, paths.get_query(), paths.get_query(true));
        }
    }
}

// TYPED_TEST(DBGAlignerPostChainTest, align_chain_swap) {
//     size_t k = 5;
//     std::string reference = "ATGATATGATGACCCCGG";
//     std::string query     = "TGACCCCGGATGATATGA";

//     auto graph = build_graph_batch<TypeParam>(k, { reference });
//     DBGAlignerConfig config;
//     config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
//     config.post_chain_alignments = true;

//     DBGAligner<> aligner(*graph, config);
//     auto paths = aligner.align(query);
//     check_chain(paths, *graph, config);
//     ASSERT_EQ(1u, paths.size());
//     EXPECT_EQ(std::string("TGACCCCGGATGATATGA"), paths[0].get_sequence());
//     check_extend(graph, aligner.get_config(), paths, query);
// }

TYPED_TEST(DBGAlignerPostChainTest, align_chain_overlap_2) {
    size_t k = 5;
    std::string reference1 = "TGAGGATCAG";
    std::string reference2 =        "CAGCTAGCTAGCTAGC";
    std::string query      = "TGAGGATCAGCTAGCTAGCTAGC";

    auto graph = build_graph_batch<TypeParam>(k, { reference1, reference2 });
    DBGAlignerConfig config;
    config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
    // config.post_chain_alignments = true;
    config.min_seed_length = 3;
    config.max_seed_length = 3;

    DBGAligner<> aligner(*graph, config);
    auto paths = aligner.align(query);
    check_chain(paths, *graph, config);
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(std::string("TGAGGATCAGCTAGCTAGCTAGC"), paths[0].get_sequence());
}

TYPED_TEST(DBGAlignerPostChainTest, align_chain_overlap_mismatch) {
    size_t k = 8;
    std::string reference1 = "TTCCTGAGGATCCG";
    std::string reference2 =        "GGATCAGCTAGCTAGCTAGC";
    std::string query      = "TTCCTGAGGATCTGCTAGCTAGCTAGC";

    auto graph = build_graph_batch<TypeParam>(k, { reference1, reference2 });
    DBGAlignerConfig config;
    config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
    // config.post_chain_alignments = true;
    config.forward_and_reverse_complement = true;
    config.min_seed_length = 5;
    config.max_seed_length = 5;

    DBGAligner<> aligner(*graph, config);
    auto paths = aligner.align(query);
    check_chain(paths, *graph, config);
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(std::string("TTCCTGAGGATCAGCTAGCTAGCTAGC"), paths[0].get_sequence());
}

TYPED_TEST(DBGAlignerPostChainTest, align_chain_overlap_3_prefer_mismatch_over_gap) {
    size_t k = 5;
    std::string reference1 = "TTTTGAGGATCAG";
    std::string reference2 =           "CAGGTTATTAGCT";
    std::string reference3 =                     "GCTTGCTAGC";
    std::string query      = "TTTTGAGGATCAGCTTATTAGCTTGCTAGC";
    //                                     X

    auto graph = build_graph_batch<TypeParam>(k, { reference1, reference2, reference3 });
    DBGAlignerConfig config;
    config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -3, -3);
    config.min_seed_length = 3;
    config.max_seed_length = 3;
    // config.post_chain_alignments = true;

    DBGAligner<> aligner(*graph, config);
    auto paths = aligner.align(query);
    check_chain(paths, *graph, config);
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(std::string("TTTTGAGGATCAGGTTATTAGCTTGCTAGC"), paths[0].get_sequence());
}

TYPED_TEST(DBGAlignerPostChainTest, align_chain_delete_no_chain_if_full_coverage) {
    size_t k = 10;
    std::string reference = "TGAGGATCAGTTCTAGCTTGCTAGC";
    std::string query     = "TGAGGATCAG""CTAGCTTGCTAGC";

    auto graph = build_graph_batch<TypeParam>(k, { reference });
    DBGAlignerConfig config;
    config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
    // config.post_chain_alignments = true;

    DBGAligner<> aligner(*graph, config);
    auto paths = aligner.align(query);
    check_chain(paths, *graph, config, false);
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(reference, paths[0].get_sequence());
}

// TODO: these three tests only makes sense if NODE_INSERTION is scored affinely
// TYPED_TEST(DBGAlignerPostChainTest, align_chain_delete1) {
//     size_t k = 10;
//     std::string reference1 = "TGAGGATCAGTTCTAGCTTG";
//     std::string reference2 =             "CTAGCTTGCTAGCGCTAGCTAGATC";
//     std::string query      = "TGAGGATCAG""CTAGCTTGCTAGCGCTAGCTAGATC";

//     auto graph = std::make_shared<DBGSuccinct>(k);
//     DBGAlignerConfig config;
//     config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
//     config.post_chain_alignments = true;
//     graph->add_sequence(reference1);
//     graph->add_sequence(reference2);

//     DBGAligner<> aligner(*graph, config);
//     auto paths = aligner.align(query);
//     check_chain(paths, *graph, config);
//     ASSERT_EQ(1u, paths.size());
//     EXPECT_EQ(std::string("TGAGGATCAGTTCTAGCTTGCTAGCGCTAGCTAGATC"), paths[0].get_sequence());
//     check_extend(graph, aligner.get_config(), paths, query);
// }

// TYPED_TEST(DBGAlignerPostChainTest, align_chain_delete_mismatch) {
//     size_t k = 10;
//     std::string reference1 = "TGAGGATCAGTTCTAGCTTG";
//     std::string reference2 =             "CTAGCTTGCTAGCGCTAGCTAGATC";
//     std::string query      = "TGAGGATCAG""CTTGCTTGCTAGCGCTAGCTAGATC";
//     //                                      X

//     auto graph = std::make_shared<DBGSuccinct>(k);
//     DBGAlignerConfig config;
//     config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
//     config.post_chain_alignments = true;
//     graph->add_sequence(reference1);
//     graph->add_sequence(reference2);

//     DBGAligner<> aligner(*graph, config);
//     auto paths = aligner.align(query);
//     check_chain(paths, *graph, config);
//     ASSERT_EQ(1u, paths.size());
//     EXPECT_EQ(std::string("TGAGGATCAGTTCTAGCTTGCTAGCGCTAGCTAGATC"), paths[0].get_sequence());
//     check_extend(graph, aligner.get_config(), paths, query);
// }

// TYPED_TEST(DBGAlignerPostChainTest, align_chain_overlap_with_insert) {
//     size_t k = 10;
//     std::string reference1 = "TGAGGATCAGTTCTAGCTTG";
//     std::string reference2 =              "CTAGCTTGCTAGCGCTAGCTAGATC";
//     std::string query      = "TGAGGATCAGTTCTAAGCTTGCTAGCGCTAGCTAGATC";

//     auto graph = std::make_shared<DBGSuccinct>(k);
//     DBGAlignerConfig config;
//     config.gap_opening_penalty = -1;
//     config.gap_extension_penalty = -1;
//     config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(1, -1, -1);
//     config.post_chain_alignments = true;
//     graph->add_sequence(reference1);
//     graph->add_sequence(reference2);

//     DBGAligner<> aligner(*graph, config);
//     auto paths = aligner.align(query);
//     check_chain(paths, *graph, config);
//     ASSERT_EQ(1u, paths.size());
//     EXPECT_EQ(std::string("TGAGGATCAGTTCTAGCTTGCTAGCGCTAGCTAGATC"), paths[0].get_sequence());
//     check_extend(graph, aligner.get_config(), paths, query);
// }

TYPED_TEST(DBGAlignerPostChainTest, align_chain_deletion_in_overlapping_node) {
    size_t k = 10;
    std::string reference1 = "TTGAGGATCAGTTCTAAGCTTG";
    std::string reference2 =                 "AGCTTGCTAGCGCTAGCTAGATC";
    std::string query      = "TTGAGGATCAG""CTAAGCTTGCTAGCGCTAGCTAGATC";

    auto graph = build_graph_batch<TypeParam>(k, { reference1, reference2 });
    DBGAlignerConfig config;
    config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
    // config.post_chain_alignments = true;
    config.min_seed_length = 5;
    config.max_seed_length = 5;

    DBGAligner<> aligner(*graph, config);
    auto paths = aligner.align(query);
    check_chain(paths, *graph, config);
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(std::string("TTGAGGATCAGTTCTAAGCTTGCTAGCGCTAGCTAGATC"), paths[0].get_sequence());
}

TYPED_TEST(DBGAlignerPostChainTest, align_chain_large_overlap) {
    size_t k = 10;
    std::string reference1 = "TGAGGATCAGTTCTAGCTTG";
    std::string reference2 =      "ATCAGTTCTAGCTTGCTAGCGCTAGCTAGATC";
    std::string query      = "TGAGGATCAGTAATCTAGCTTGCTAGCGCTAGCTAGATC";

    auto graph = build_graph_batch<TypeParam>(k, { reference1, reference2 });
    DBGAlignerConfig config;
    config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
    // config.post_chain_alignments = true;

    DBGAligner<> aligner(*graph, config);
    auto paths = aligner.align(query);
    check_chain(paths, *graph, config, false);
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(std::string("TGAGGATCAGTTCTAGCTTGCTAGCGCTAGCTAGATC"), paths[0].get_sequence());
}

TYPED_TEST(DBGAlignerPostChainTest, align_chain_delete_in_overlap) {
    size_t k = 10;
    std::string reference1 = "TGAGGATCAGTTCTAGCTTG";
    std::string reference2 =              "TAGCTTGCTAGCGCTAGCTAGATC";
    std::string query      = "TGAGGATCAGTTCTACTTGCTAGCGCTAGCTAGATC";

    auto graph = build_graph_batch<TypeParam>(k, { reference1, reference2 });
    DBGAlignerConfig config;
    config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
    config.min_seed_length = 4;
    config.max_seed_length = 4;
    // config.post_chain_alignments = true;

    DBGAligner<> aligner(*graph, config);
    auto paths = aligner.align(query);
    check_chain(paths, *graph, config);
    ASSERT_EQ(1u, paths.size());
    EXPECT_EQ(std::string("TGAGGATCAGTTCTAGCTTGCTAGCGCTAGCTAGATC"), paths[0].get_sequence());
}

// TYPED_TEST(DBGAlignerPostChainTest, align_chain_disjoint) {
//     size_t k = 10;
//     std::string reference1 = "CCCCCCCCTGAGGATCAG";
//     std::string reference2 =                   "TTCACTAGCTAGCCCCCCCCC";
//     std::string query      = "CCCCCCCCTGAGGATCAGTTCACTAGCTAGCCCCCCCCC";

//     auto graph = std::make_shared<DBGSuccinct>(k);
//     DBGAlignerConfig config;
//     config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(2, -1, -2);
//     config.post_chain_alignments = true;
//     graph->add_sequence(reference1);
//     graph->add_sequence(reference2);

//     DBGAligner<> aligner(*graph, config);
//     auto paths = aligner.align(query);
//     check_chain(paths, *graph, config);
//     ASSERT_EQ(1u, paths.size());
//     EXPECT_EQ(std::string("CCCCCCCCTGAGGATCAG$TTCACTAGCTAGCCCCCCCCC"), paths[0].get_sequence());
//     // check_extend(graph, aligner.get_config(), paths, query);
// }

// TYPED_TEST(DBGAlignerPostChainTest, align_chain_gap) {
//     size_t k = 10;
//     std::string reference1 = "AAAAACCCCCTGAGGATCAG";
//     std::string reference2 =                        "ACTAGCTAGCCCCCCAAAAA";
//     std::string query      = "AAAAACCCCCTGAGGATCAGTTCACTAGCTAGCCCCCCAAAAA";

//     auto graph = std::make_shared<DBGSuccinct>(k);
//     DBGAlignerConfig config;
//     config.gap_opening_penalty = -1;
//     config.gap_extension_penalty = -1;
//     config.score_matrix = DBGAlignerConfig::dna_scoring_matrix(1, -1, -1);
//     config.post_chain_alignments = true;
//     graph->add_sequence(reference1);
//     graph->add_sequence(reference2);

//     DBGAligner<> aligner(*graph, config);
//     auto paths = aligner.align(query);
//     check_chain(paths, *graph, config);
//     ASSERT_EQ(1u, paths.size());
//     EXPECT_EQ(std::string("AAAAACCCCCTGAGGATCAG$ACTAGCTAGCCCCCCAAAAA"), paths[0].get_sequence());
//     // check_extend(graph, aligner.get_config(), paths, query);
// }

} // namespace
