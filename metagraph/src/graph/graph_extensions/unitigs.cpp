#include "unitigs.hpp"

#include <mutex>

#include <progress_bar.hpp>

#include "cli/load/load_graph.hpp"
#include "annotation/representation/column_compressed/annotate_column_compressed.hpp"
#include "common/utils/file_utils.hpp"
#include "common/unix_tools.hpp"
#include "common/vectors/bit_vector_dyn.hpp"
#include "annotation/annotation_converters.hpp"
#include "graph/alignment/aligner_labeled.hpp"
#include "graph/annotated_dbg.hpp"
#include "common/threads/threading.hpp"
#include "common/serialization.hpp"


namespace mtg {
namespace graph {
namespace align {

using common::logger;

Unitigs::Unitigs(const DBGSuccinct &graph)
      : graph_(std::shared_ptr<const DeBruijnGraph>{}, &graph) {
    if (graph_->get_mode() == DeBruijnGraph::PRIMARY)
        canonical_ = std::make_unique<CanonicalDBG>(graph_);

    if (graph_->get_mask())
        throw std::runtime_error("Masked graphs not supported");
}

Unitigs::Unitigs(const std::string &graph_fname,
                 size_t max_path_length,
                 double memory_available,
                 const std::string &tmp_dir_path) : graph_(load_graph_impl(graph_fname)) {
    if (graph_->get_mode() == DeBruijnGraph::CANONICAL)
        throw std::runtime_error("CANONICAL mode graphs not supported");

    std::filesystem::path tmp_dir = utils::create_temp_dir(tmp_dir_path, "unitigs");
    std::string out_path = tmp_dir/"unitigs";
    std::vector<std::string> files;
    size_t width = sdsl::bits::hi(graph_->num_nodes()) + 1;
    std::vector<size_t> unitigs;
    std::vector<std::unique_ptr<bit_vector>> cols;

    {
        DBGSuccinct &ncgraph = const_cast<DBGSuccinct&>(*graph_);
        if (!graph_->get_mask()) {
            logger->trace("Marking dummy k-mers");
            ncgraph.mask_dummy_kmers(get_num_threads(), false);
        }

        valid_nodes_.reset(ncgraph.release_mask());
    }

    auto colcomp = std::make_unique<annot::ColumnCompressed<>>(graph_->num_nodes(), 1, tmp_dir, 1e9, width);

    std::vector<std::string> labels;
    labels.push_back("");

    logger->trace("Annotating unitigs");
    std::mutex mu;
    size_t counter = 0;
    size_t max_unitig = 0;

    ThreadPool pool(1);
    graph_->call_unitigs([&](const std::string&, const auto &path) {
        if (path.size() == 1)
            return;

        std::vector<annot::ColumnCompressed<>::Index> rows;
        rows.reserve(path.size());
        for (node_index node : path) {
            rows.push_back(AnnotatedDBG::graph_to_anno_index(node));
        }

        pool.enqueue([&colcomp,&labels,r=std::move(rows)]() {
            colcomp->add_labels(r, labels);
        });

    }, get_num_threads());
    pool.join();

    graph_->call_unitigs([&](const std::string&, const auto &path) {
        if (path.size() == 1)
            return;

        size_t start_coord;
        {
            std::lock_guard<std::mutex> lock(mu);
            start_coord = counter;
            unitigs.emplace_back(path.front());
            unitigs.emplace_back(path.back());
            unitigs.emplace_back(counter);
            max_unitig = std::max({ path.front(), path.back(), max_unitig });
            counter += path.size();
        }

        std::vector<std::pair<annot::ColumnCompressed<>::Index, uint64_t>> coords;
        for (size_t i = 0; i < path.size(); ++i) {
            coords.emplace_back(AnnotatedDBG::graph_to_anno_index(path[i]), i + start_coord);
        }

        pool.enqueue([&colcomp,&labels,c=std::move(coords)]() {
            colcomp->add_label_coords(c, labels);
        });
    }, get_num_threads());

    pool.join();

    logger->trace("Initializing unitig vector");
    size_t num_unitigs = unitigs.size() / 3;
    // TODO: replace with int_vector_buffer
    sdsl::int_vector<> boundaries(num_unitigs * 2, 0, sdsl::bits::hi(max_unitig + 1) + 1);
    indicator_ = Indicator([&](const auto &callback) {
        ProgressBar progress_bar(num_unitigs, "Packing unitigs",
                                 std::cerr, !common::get_verbose());
        for (size_t i = 0, j = 0; i < unitigs.size(); i += 3, j += 2) {
            boundaries[j] = unitigs[i];
            boundaries[j + 1] = unitigs[i + 1];
            callback(unitigs[i + 2]);
            ++progress_bar;
        }
    }, counter, num_unitigs);
    boundaries_ = IDVector(std::move(boundaries));
    assert(indicator_.num_set_bits() * 2 == boundaries_.size());
    unitigs = std::vector<size_t>();

    logger->trace("Serializing initial annotation");
    files.push_back(out_path + colcomp->file_extension());
    colcomp->serialize(files[0]);
    colcomp.reset();
    graph_.reset();
    logger->trace("Compressing unitig index");
    logger->trace("Step 0");
    convert_to_row_diff(files,
                        graph_fname,
                        memory_available * 1e9,
                        max_path_length,
                        tmp_dir,
                        tmp_dir,
                        static_cast<annot::RowDiffStage>(0),
                        out_path + ".row_count", false, true);
    logger->trace("Step 1");
    convert_to_row_diff(files,
                        graph_fname,
                        memory_available * 1e9,
                        max_path_length,
                        tmp_dir,
                        tmp_dir,
                        static_cast<annot::RowDiffStage>(1),
                        out_path + ".row_reduction", false, true);
    logger->trace("Step 2");
    convert_to_row_diff(files,
                        graph_fname,
                        memory_available * 1e9,
                        max_path_length,
                        tmp_dir,
                        tmp_dir,
                        static_cast<annot::RowDiffStage>(2),
                        out_path + ".row_reduction", false, true);
    logger->trace("done");
    const std::string anchors_file = graph_fname + annot::binmat::kRowDiffAnchorExt;
    if (!std::filesystem::exists(anchors_file)) {
        logger->error("Anchor bitmap {} does not exist.", anchors_file);
        std::exit(1);
    }
    const std::string fork_succ_file = graph_fname + annot::binmat::kRowDiffForkSuccExt;
    if (!std::filesystem::exists(fork_succ_file)) {
        logger->error("Fork successor bitmap {} does not exist", fork_succ_file);
        std::exit(1);
    }

    logger->trace("Loading column");
    auto annotator = std::make_unique<annot::ColumnCompressed<>>(0);
    if (!annotator->merge_load(files)) {
        logger->error("Cannot load annotations");
        exit(1);
    }

    logger->trace("Wrapping as TupleRowDiff");

    std::vector<bit_vector_smart> delimiters(1);
    std::vector<sdsl::int_vector<>> column_values(1);

    auto coords_fname = utils::remove_suffix(files[0], annot::ColumnCompressed<>::kExtension)
                                                    + annot::ColumnCompressed<>::kCoordExtension;
    typedef annot::ColumnCoordAnnotator::binary_matrix_type UnitigCoordDiff;
    std::ifstream in(coords_fname, std::ios::binary);
    try {
        UnitigCoordDiff::load_tuples(in, 1, [&](auto&& delims, auto&& values) {
            delimiters[0] = std::move(delims);
            column_values[0] = std::move(values);
        });
    } catch (const std::exception &e) {
        logger->error("Couldn't load coordinates from {}\nException: {}", coords_fname, e.what());
        exit(1);
    } catch (...) {
        logger->error("Couldn't load coordinates from {}", coords_fname);
        exit(1);
    }

    unitigs_ = UnitigCoords(nullptr,
                            UnitigCoordDiff(std::move(*annotator->release_matrix()),
                                            std::move(delimiters),
                                            std::move(column_values)));

    unitigs_.load_anchor(anchors_file);
    unitigs_.load_fork_succ(fork_succ_file);
    logger->trace("RowDiff support bitmaps loaded");
    load_graph(graph_fname);
    unitigs_.set_graph(graph_.get());
}

bool Unitigs::load(const std::string &filename_base) {
    std::string fname = utils::make_suffix(filename_base, kUnitigsExtension);
    std::ifstream fin(fname, std::ios::binary);
    if (!fin.good()) {
        logger->error("Failed to read unitig index from {}", fname);
        return false;
    }

    if (!unitigs_.load(fin)) {
        logger->error("Failed to read unitig coordinates");
        return false;
    }

    unitigs_.set_graph(graph_.get());
    valid_nodes_ = graph_->load_mask(fin);
    if (!valid_nodes_) {
        logger->error("Failed to load valid node indicator");
        return false;
    }

    try {
        boundaries_.load(fin);
    } catch (...) {
        logger->error("Failed to load unitig boundary array");
        return false;
    }

    if (boundaries_.size() % 2) {
        logger->error("Unitig boundary array should be of even length: {}",
                      boundaries_.size());
        return false;
    }

    if (!indicator_.load(fin)) {
        logger->error("Failed to load coordinate boundary indicator");
        return false;
    }

    if (indicator_.num_set_bits() * 2 != boundaries_.size()) {
        logger->error("Unitig storage is inconsistent: {} indicators vs. {} unitigs",
                      indicator_.num_set_bits(), boundaries_.size() / 2);
        return false;
    }

    DEBUG_LOG("Loaded unitig index with {} unitigs and {} coordinates",
              boundaries_.size() / 2, valid_nodes_->num_set_bits());

    return true;
}

void Unitigs::serialize(const std::string &filename_base) const {
    std::string fname = utils::make_suffix(filename_base, kUnitigsExtension);
    std::ofstream fout(fname, std::ios::binary);
    unitigs_.serialize(fout);
    valid_nodes_->serialize(fout);
    boundaries_.serialize(fout);
    indicator_.serialize(fout);
}

auto Unitigs::get_unitig(size_t unitig_id) const -> std::pair<node_index, node_index> {
    size_t unitig_id_offset = get_unitig_id_offset();
    if (unitig_id <= unitig_id_offset)
        return std::make_pair(unitig_id, unitig_id);

    unitig_id -= unitig_id_offset;
    bool is_rc_unitig = false;
    size_t rc_unitig_offset = get_rc_unitig_offset();
    if (unitig_id > rc_unitig_offset) {
        unitig_id -= rc_unitig_offset;
        is_rc_unitig = true;
    }

    node_index first = boundaries_[(unitig_id - 1) * 2];
    node_index second = boundaries_[(unitig_id - 1) * 2 + 1];

    if (is_rc_unitig) {
        assert(canonical_);
        first = canonical_->reverse_complement(first);
        second = canonical_->reverse_complement(second);
        std::swap(first, second);
    }

    return std::make_pair(first, second);
}

auto Unitigs::get_unitig_bounds(size_t unitig_id) const
        -> std::pair<std::pair<node_index, node_index>, std::pair<Coord, Coord>> {
    size_t unitig_id_offset = get_unitig_id_offset();
    if (unitig_id <= unitig_id_offset) {
        return std::make_pair(std::make_pair(unitig_id, unitig_id),
                              std::make_pair(0, 1));
    }

    unitig_id -= unitig_id_offset;
    bool is_rc_unitig = false;
    size_t rc_unitig_offset = get_rc_unitig_offset();
    if (unitig_id > rc_unitig_offset) {
        unitig_id -= rc_unitig_offset;
        is_rc_unitig = true;
    }

    node_index first = boundaries_[(unitig_id - 1) * 2];
    node_index second = boundaries_[(unitig_id - 1) * 2 + 1];
    Coord first_c = indicator_.select1(unitig_id);
    Coord second_c = unitig_id + 1 <= indicator_.num_set_bits()
                                             ? indicator_.select1(unitig_id + 1)
                                             : indicator_.size();

    if (is_rc_unitig) {
        assert(canonical_);
        first = canonical_->reverse_complement(first);
        second = canonical_->reverse_complement(second);
        std::swap(first, second);
        first_c += indicator_.size();
        second_c += indicator_.size();
    }

    return std::make_pair(std::make_pair(first, second),
                          std::make_pair(first_c, second_c));
}

void Unitigs::adjacent_outgoing_unitigs(size_t unitig_id,
                                        const std::function<void(size_t)> &callback) const {
    const DeBruijnGraph *graph = graph_.get();
    if (canonical_)
        graph = canonical_.get();

    graph->adjacent_outgoing_nodes(get_unitig(unitig_id).second, [&](node_index next) {
        auto next_unitig_ids = get_unitig_ids({ next });
        callback(next_unitig_ids.size() ? next_unitig_ids[0] : next);
    });
}

void Unitigs::adjacent_incoming_unitigs(size_t unitig_id,
                                        const std::function<void(size_t)> &callback) const {
    const DeBruijnGraph *graph = graph_.get();
    if (canonical_)
        graph = canonical_.get();

    graph->adjacent_incoming_nodes(get_unitig(unitig_id).second, [&](node_index prev) {
        auto prev_unitig_ids = get_unitig_ids({ prev });
        callback(prev_unitig_ids.size() ? prev_unitig_ids[0] : prev);
    });
}

std::vector<size_t> Unitigs::get_unitig_ids(const std::vector<node_index> &nodes) const {
    auto [indicator, is_rc_node, rows] = nodes_to_rows(nodes);
    auto seed_tuples = unitigs_.get_row_tuples(rows);
    std::vector<size_t> results;
    results.reserve(nodes.size());

    size_t j = 0;
    size_t unitig_id_offset = get_unitig_id_offset();
    size_t rc_unitig_offset = get_rc_unitig_offset();
    for (size_t i = 0; i < nodes.size(); ++i) {
        size_t unitig_id = std::numeric_limits<size_t>::max();
        if (indicator[i]) {
            const auto &coordinates = seed_tuples[j++];
            if (coordinates.size()) {
                assert(coordinates.size() == 1);
                assert(!coordinates[0].first);
                assert(coordinates[0].second.size() == 1);
                unitig_id = indicator_.rank1(coordinates[0].second[0]);
                results.emplace_back(unitig_id + unitig_id_offset
                                        + (rc_unitig_offset * is_rc_node[i]));
            }
        }

        if (unitig_id == std::numeric_limits<size_t>::max())
            results.emplace_back(nodes[i]);
    }

    return results;
}

auto Unitigs::get_unitig_ids_and_coordinates(const std::vector<node_index> &nodes) const
        -> std::vector<std::pair<size_t, Coord>> {
    auto [indicator, is_rc_node, rows] = nodes_to_rows(nodes);
    auto seed_tuples = unitigs_.get_row_tuples(rows);
    std::vector<std::pair<size_t, Coord>> results;
    results.reserve(nodes.size());

    size_t j = 0;
    size_t unitig_id_offset = get_unitig_id_offset();
    size_t rc_unitig_offset = get_rc_unitig_offset();
    for (size_t i = 0; i < nodes.size(); ++i) {
        size_t unitig_id = std::numeric_limits<size_t>::max();
        if (indicator[i]) {
            const auto &coordinates = seed_tuples[j];
            if (coordinates.size()) {
                assert(coordinates.size() == 1);
                assert(!coordinates[0].first);
                assert(coordinates[0].second.size() == 1);
                unitig_id = indicator_.rank1(coordinates[0].second[0]);
                results.emplace_back(unitig_id + unitig_id_offset
                                        + (rc_unitig_offset * is_rc_node[i]),
                                     coordinates[0].second[0]);
            }
            ++j;
        }

        if (unitig_id == std::numeric_limits<size_t>::max())
            results.emplace_back(nodes[i], 0);
    }

    return results;
}

void Unitigs::load_graph(const std::string &fname) {
    graph_ = load_graph_impl(fname);
    const_cast<DBGSuccinct&>(*graph_).reset_mask();
    unitigs_.set_graph(graph_.get());
}

std::shared_ptr<const DBGSuccinct> Unitigs::load_graph_impl(const std::string &fname) {
    logger->trace("Graph loading...");
    Timer timer;
    return std::dynamic_pointer_cast<const DBGSuccinct>(cli::load_critical_dbg(fname));
    logger->trace("Graph loaded in {} sec", timer.elapsed());
}

std::tuple<sdsl::bit_vector, sdsl::bit_vector, std::vector<uint64_t>>
Unitigs::nodes_to_rows(const std::vector<node_index> &nodes) const {
    sdsl::bit_vector indicator(nodes.size(), false);
    sdsl::bit_vector is_rc_node(nodes.size(), false);
    std::vector<uint64_t> rows;
    for (size_t i = 0; i < nodes.size(); ++i) {
        node_index base_node = get_base_node(nodes[i]);
        is_rc_node[i] = (nodes[i] != base_node);

        if ((*valid_nodes_)[graph_->kmer_to_boss_index(base_node)]) {
            indicator[i] = true;
            rows.emplace_back(AnnotatedDBG::graph_to_anno_index(base_node));
        }
    }

    return std::make_tuple(std::move(indicator), std::move(is_rc_node), std::move(rows));
}

size_t Unitigs::get_unitig_id_offset() const {
    return (canonical_ ? canonical_->max_index() : graph_->max_index()) + 1;
}

size_t Unitigs::get_rc_unitig_offset() const { return boundaries_.size() / 2; }

auto Unitigs::get_base_node(node_index node) const -> node_index {
    return canonical_ ? canonical_->get_base_node(node) : node;
}

} // namespace align
} // namespace graph
} // namespace mtg
