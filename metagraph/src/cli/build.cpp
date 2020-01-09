#include "build.hpp"

#include "common/logger.hpp"
#include "common/algorithms.hpp"
#include "common/unix_tools.hpp"
#include "common/threads/threading.hpp"
#include "graph/representation/hash/dbg_hash_ordered.hpp"
#include "graph/representation/hash/dbg_hash_string.hpp"
#include "graph/representation/hash/dbg_hash_fast.hpp"
#include "graph/representation/bitmap/dbg_bitmap.hpp"
#include "graph/representation/bitmap/dbg_bitmap_construct.hpp"
#include "graph/representation/succinct/dbg_succinct.hpp"
#include "graph/representation/succinct/boss_construct.hpp"
#include "graph/graph_extensions/node_weights.hpp"
#include "config/config.hpp"
#include "sequence_reader.hpp"

using mg::common::logger;
using utils::get_verbose;
using namespace mg::bitmap_graph;
using namespace mg::succinct;

const uint64_t kBytesInGigabyte = 1'000'000'000;


int build_graph(Config *config) {
    assert(config);

    const auto &files = config->fnames;

    std::unique_ptr<DeBruijnGraph> graph;

    logger->trace("Build De Bruijn Graph with k-mer size k={}", config->k);

    Timer timer;

    if (config->canonical)
        config->forward_and_reverse = false;

    if (config->complete) {
        if (config->graph_type != Config::GraphType::BITMAP) {
            logger->error("Only bitmap-graph can be built in complete mode");
            exit(1);
        }

        graph.reset(new DBGBitmap(config->k, config->canonical));

    } else if (config->graph_type == Config::GraphType::SUCCINCT && !config->dynamic) {
        auto boss_graph = std::make_unique<BOSS>(config->k - 1);

        logger->trace("Start reading data and extracting k-mers");
        //enumerate all suffixes
        assert(boss_graph->alph_size > 1);
        std::vector<std::string> suffixes;
        if (config->suffix.size()) {
            suffixes = { config->suffix };
        } else {
            suffixes = KmerExtractorBOSS::generate_suffixes(config->suffix_len);
        }

        BOSS::Chunk graph_data(KmerExtractorBOSS::alphabet.size(),
                               boss_graph->get_k(),
                               config->canonical);

        //one pass per suffix
        for (const std::string &suffix : suffixes) {
            timer.reset();

            if (suffix.size() > 0 || suffixes.size() > 1) {
                logger->info("k-mer suffix: '{}'", suffix);
            }

            auto constructor = IBOSSChunkConstructor::initialize(
                boss_graph->get_k(),
                config->canonical,
                config->count_width,
                suffix,
                get_num_threads(),
                config->memory_available * kBytesInGigabyte,
                config->container
            );

            parse_sequences(files, *config, timer,
                [&](std::string&& read) { constructor->add_sequence(std::move(read)); },
                [&](std::string&& kmer, uint32_t count) { constructor->add_sequence(std::move(kmer), count); },
                [&](const auto &loop) { constructor->add_sequences(loop); }
            );

            auto next_block = constructor->build_chunk();
            logger->trace("Graph chunk with {} k-mers was built in {} sec",
                          next_block->size(), timer.elapsed());

            if (config->outfbase.size() && config->suffix.size()) {
                logger->info("Serialize the graph chunk for suffix '{}'...", suffix);
                timer.reset();
                next_block->serialize(config->outfbase + "." + suffix);
                logger->info("Serialization done in {} sec", timer.elapsed());
            }

            if (config->suffix.size())
                return 0;

            graph_data.extend(*next_block);
            delete next_block;
        }

        if (config->count_kmers) {
            sdsl::int_vector<> kmer_counts;
            graph_data.initialize_boss(boss_graph.get(), &kmer_counts);
            graph.reset(new DBGSuccinct(boss_graph.release(), config->canonical));
            graph->add_extension(std::make_shared<NodeWeights>(std::move(kmer_counts)));
            assert(graph->get_extension<NodeWeights>()->is_compatible(*graph));
        } else {
            graph_data.initialize_boss(boss_graph.get());
            graph.reset(new DBGSuccinct(boss_graph.release(), config->canonical));
        }

    } else if (config->graph_type == Config::GraphType::BITMAP && !config->dynamic) {

        if (!config->outfbase.size()) {
            logger->error("No output file provided");
            exit(1);
        }

        logger->trace("Start reading data and extracting k-mers");
        // enumerate all suffixes
        std::vector<std::string> suffixes;
        if (config->suffix.size()) {
            suffixes = { config->suffix };
        } else {
            suffixes = KmerExtractor2Bit().generate_suffixes(config->suffix_len);
        }

        std::unique_ptr<DBGBitmapConstructor> constructor;
        std::vector<std::string> chunk_filenames;

        //one pass per suffix
        for (const std::string &suffix : suffixes) {
            timer.reset();

            if ((suffix.size() > 0 || suffixes.size() > 1)) {
                logger->trace("k-mer suffix: '{}'", suffix);
            }

            constructor.reset(
                new DBGBitmapConstructor(
                    config->k,
                    config->canonical,
                    config->count_width,
                    suffix,
                    get_num_threads(),
                    config->memory_available * kBytesInGigabyte
                )
            );

            parse_sequences(files, *config, timer,
                [&](std::string&& read) { constructor->add_sequence(std::move(read)); },
                [&](std::string&& kmer, uint32_t count) { constructor->add_sequence(std::move(kmer), count); },
                [&](const auto &loop) { constructor->add_sequences(loop); }
            );

            if (!suffix.size()) {
                assert(suffixes.size() == 1);

                auto *bitmap_graph = new DBGBitmap(config->k);
                constructor->build_graph(bitmap_graph);
                graph.reset(bitmap_graph);

            } else {
                std::unique_ptr<DBGBitmap::Chunk> chunk { constructor->build_chunk() };
                logger->trace("Graph chunk with {} k-mers was built in {} sec",
                              chunk->num_set_bits(), timer.elapsed());

                logger->trace("Serialize the graph chunk for suffix '{}'...", suffix);

                chunk_filenames.push_back(
                        utils::join_strings({ config->outfbase, suffix }, ".")
                        + DBGBitmap::kChunkFileExtension
                );
                std::ofstream out(chunk_filenames.back(), std::ios::binary);
                chunk->serialize(out);
                logger->trace("Serialization done in {} sec", timer.elapsed());
            }

            // only one chunk had to be constructed
            if (config->suffix.size())
                return 0;
        }

        if (suffixes.size() > 1) {
            assert(chunk_filenames.size());
            timer.reset();
            graph.reset(constructor->build_graph_from_chunks(chunk_filenames,
                                                             config->canonical,
                                                             get_verbose()));
        }

    } else {
        //slower method
        switch (config->graph_type) {

            case Config::GraphType::SUCCINCT:
                graph.reset(new DBGSuccinct(config->k, config->canonical));
                break;

            case Config::GraphType::HASH:
                graph.reset(new DBGHashOrdered(config->k, config->canonical));
                break;

            case Config::GraphType::HASH_PACKED:
                graph.reset(new DBGHashOrdered(config->k, config->canonical, true));
                break;

            case Config::GraphType::HASH_FAST:
                graph.reset(new DBGHashFast(config->k, config->canonical, true));
                break;

            case Config::GraphType::HASH_STR:
                if (config->canonical) {
                    logger->warn("String hash-based de Bruijn graph"
                                 " does not support canonical mode."
                                 " Normal mode will be used instead.");
                }
                // TODO: implement canonical mode
                graph.reset(new DBGHashString(config->k/*, config->canonical*/));
                break;

            case Config::GraphType::BITMAP:
                logger->error("Bitmap-graph construction"
                              " in dynamic regime is not supported");
                exit(1);

            case Config::GraphType::INVALID:
                assert(false);
        }
        assert(graph);

        parse_sequences(files, *config, timer,
            [&graph](std::string&& seq) {
                graph->add_sequence(std::move(seq));
            },
            [&graph](std::string&& kmer, uint32_t /*count*/) {
                graph->add_sequence(std::move(kmer));
            },
            [&graph](const auto &loop) {
                loop([&graph](const char *seq) { graph->add_sequence(seq); });
            }
        );

        if (config->count_kmers) {
            graph->add_extension(std::make_shared<NodeWeights>(graph->max_index() + 1,
                                                               config->count_width));
            auto node_weights = graph->get_extension<NodeWeights>();
            assert(node_weights->is_compatible(*graph));

            if (graph->is_canonical_mode())
                config->forward_and_reverse = true;

            parse_sequences(files, *config, timer,
                [&graph,&node_weights](std::string&& seq) {
                    graph->map_to_nodes_sequentially(seq,
                        [&](auto node) { node_weights->add_weight(node, 1); }
                    );
                },
                [&graph,&node_weights](std::string&& kmer, uint32_t count) {
                    node_weights->add_weight(graph->kmer_to_node(kmer), count);
                },
                [&graph,&node_weights](const auto &loop) {
                    loop([&graph,&node_weights](const char *seq) {
                        std::string seq_str(seq);
                        graph->map_to_nodes_sequentially(seq_str,
                            [&](auto node) { node_weights->add_weight(node, 1); }
                        );
                    });
                }
            );
        }
    }

    logger->trace("Graph construction finished in {} sec", timer.elapsed());

    if (!config->outfbase.empty()) {
        if (dynamic_cast<DBGSuccinct*>(graph.get()) && config->mark_dummy_kmers) {
            logger->trace("Detecting all dummy k-mers...");

            timer.reset();
            dynamic_cast<DBGSuccinct&>(*graph).mask_dummy_kmers(get_num_threads(), false);

            logger->trace("Dummy k-mer detection done in {} sec", timer.elapsed());
        }

        graph->serialize(config->outfbase);
        graph->serialize_extensions(config->outfbase);
    }

    return 0;
}
