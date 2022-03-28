#include "annotation_buffer.hpp"

#include "graph/representation/rc_dbg.hpp"
#include "graph/representation/succinct/dbg_succinct.hpp"
#include "graph/representation/canonical_dbg.hpp"
#include "annotation/binary_matrix/base/binary_matrix.hpp"
#include "common/utils/template_utils.hpp"

namespace mtg {
namespace graph {
namespace align {

using mtg::common::logger;

typedef annot::binmat::BinaryMatrix::Row Row;
typedef annot::binmat::BinaryMatrix::Column Column;

// dummy index for an unfetched annotations
static constexpr size_t nannot = std::numeric_limits<size_t>::max();

AnnotationBuffer::AnnotationBuffer(const DeBruijnGraph &graph,
                                   const Annotator &annotator,
                                   size_t row_batch_size,
                                   size_t max_coords_per_node)
      : graph_(graph),
        annotator_(annotator),
        multi_int_(dynamic_cast<const annot::matrix::MultiIntMatrix*>(&annotator_.get_matrix())),
        canonical_(dynamic_cast<const CanonicalDBG*>(&graph_)),
        column_sets_({ {} }),
        row_batch_size_(row_batch_size),
        max_coords_per_node_(max_coords_per_node) {
    if (multi_int_ && graph_.get_mode() != DeBruijnGraph::BASIC) {
        multi_int_ = nullptr;
        logger->warn("Coordinates not supported when aligning to CANONICAL "
                     "or PRIMARY mode graphs");
    }
}

void AnnotationBuffer::fetch_queued_annotations() {
    assert(graph_.get_mode() != DeBruijnGraph::PRIMARY
                && "PRIMARY graphs must be wrapped into CANONICAL");

    const DeBruijnGraph *base_graph = &graph_;

    if (canonical_)
        base_graph = &canonical_->get_graph();

    const auto *dbg_succ = dynamic_cast<const DBGSuccinct*>(base_graph);
    const boss::BOSS *boss = dbg_succ ? &dbg_succ->get_boss() : nullptr;

    auto fetch_row_batch = [&](auto&& queued_nodes, auto&& queued_rows) {
        if (queued_nodes.empty())
            return;

        auto push_node_labels = [&](auto node_it, auto row_it, auto&& labels) {
            assert(node_it != queued_nodes.end());
            assert(node_to_cols_.count(*node_it));
            assert(node_to_cols_.count(AnnotatedDBG::anno_to_graph_index(*row_it)));

            size_t label_i = cache_column_set(std::move(labels));
            node_index base_node = AnnotatedDBG::anno_to_graph_index(*row_it);
            if (graph_.get_mode() == DeBruijnGraph::BASIC) {
                assert(base_node == *node_it);
                node_to_cols_[*node_it] = label_i;
            } else if (canonical_) {
                node_to_cols_[base_node] = label_i;
            } else {
                node_to_cols_[*node_it] = label_i;
                if (base_node != *node_it && node_to_cols_.try_emplace(base_node, label_i).second
                        && has_coordinates()) {
                    label_coords_.emplace_back(label_coords_.back());
                }
            }
        };

        auto node_it = queued_nodes.begin();
        auto row_it = queued_rows.begin();
        if (has_coordinates()) {
            assert(multi_int_);
            // extract both labels and coordinates, then store them separately
            for (auto&& row_tuples : multi_int_->get_row_tuples(queued_rows)) {
                std::sort(row_tuples.begin(), row_tuples.end(), utils::LessFirst());
                Columns labels;
                labels.reserve(row_tuples.size());
                label_coords_.emplace_back();
                label_coords_.back().reserve(row_tuples.size());
                for (auto&& [label, coords] : row_tuples) {
                    labels.push_back(label);
                    if (coords.size() <= max_coords_per_node_) {
                        label_coords_.back().emplace_back(coords.begin(), coords.end());
                    } else {
                        label_coords_.back().emplace_back();
                    }
                }
                push_node_labels(node_it++, row_it++, std::move(labels));
            }
        } else {
            for (auto&& labels : annotator_.get_matrix().get_rows(queued_rows)) {
                std::sort(labels.begin(), labels.end());
                push_node_labels(node_it++, row_it++, std::move(labels));
            }
        }
    };

    std::vector<node_index> queued_nodes;
    std::vector<Row> queued_rows;
    for (const auto &path : queued_paths_) {
        std::vector<node_index> base_path;
        if (base_graph->get_mode() == DeBruijnGraph::CANONICAL) {
            // TODO: avoid this call of spell_path
            std::string query = spell_path(graph_, path);
            base_path = map_to_nodes(*base_graph, query);

        } else if (canonical_) {
            base_path.reserve(path.size());
            for (node_index node : path) {
                base_path.emplace_back(canonical_->get_base_node(node));
            }

        } else {
            assert(graph_.get_mode() == DeBruijnGraph::BASIC);
            base_path = path;
            if (dynamic_cast<const RCDBG*>(&graph_))
                std::reverse(base_path.begin(), base_path.end());
        }

        assert(base_path.size() == path.size());

        for (size_t i = 0; i < path.size(); ++i) {
            if (base_path[i] == DeBruijnGraph::npos) {
                // this can happen when the base graph is CANONICAL and path[i] is a
                // dummy node
                if (node_to_cols_.try_emplace(path[i], 0).second && has_coordinates())
                    label_coords_.emplace_back();

                continue;
            }

            if (boss && !boss->get_W(dbg_succ->kmer_to_boss_index(base_path[i]))) {
                // skip dummy nodes
                if (node_to_cols_.try_emplace(base_path[i], 0).second && has_coordinates())
                    label_coords_.emplace_back();

                if (graph_.get_mode() == DeBruijnGraph::CANONICAL
                        && base_path[i] != path[i]
                        && node_to_cols_.emplace(path[i], 0).second && has_coordinates()) {
                    label_coords_.emplace_back();
                }

                continue;
            }

            Row row = AnnotatedDBG::graph_to_anno_index(base_path[i]);
            if (canonical_ || graph_.get_mode() == DeBruijnGraph::BASIC) {
                if (node_to_cols_.try_emplace(base_path[i], nannot).second) {
                    queued_rows.push_back(row);
                    queued_nodes.push_back(base_path[i]);
                }

                continue;
            }

            assert(graph_.get_mode() == DeBruijnGraph::CANONICAL);

            auto find_a = node_to_cols_.find(path[i]);
            auto find_b = node_to_cols_.find(base_path[i]);

            if (find_a == node_to_cols_.end() && find_b == node_to_cols_.end()) {
                node_to_cols_.try_emplace(path[i], nannot);
                queued_rows.push_back(row);
                queued_nodes.push_back(path[i]);

                if (path[i] != base_path[i]) {
                    node_to_cols_.emplace(base_path[i], nannot);
                    queued_rows.push_back(row);
                    queued_nodes.push_back(base_path[i]);
                }
            } else if (find_a == node_to_cols_.end() && find_b != node_to_cols_.end()) {
                node_to_cols_.try_emplace(path[i], find_b->second);
                if (find_b->second == nannot) {
                    queued_rows.push_back(row);
                    queued_nodes.push_back(path[i]);
                }
            } else if (find_a != node_to_cols_.end() && find_b == node_to_cols_.end()) {
                node_to_cols_.try_emplace(base_path[i], find_a->second);
            } else {
                size_t label_i = std::min(find_a->second, find_b->second);
                if (label_i != nannot) {
                    find_a.value() = label_i;
                    find_b.value() = label_i;
                }
            }

            if (queued_rows.size() >= row_batch_size_) {
                fetch_row_batch(std::move(queued_nodes), std::move(queued_rows));
                queued_nodes = decltype(queued_nodes){};
                queued_rows = decltype(queued_rows){};
            }
        }
    }

    fetch_row_batch(std::move(queued_nodes), std::move(queued_rows));

    queued_paths_.clear();
}

auto AnnotationBuffer::get_labels_and_coords(node_index node, bool skip_unfetched) const
        -> std::pair<const Columns*, std::shared_ptr<const CoordinateSet>> {
    std::pair<const Columns*, std::shared_ptr<const CoordinateSet>> ret_val;

    if (canonical_)
        node = canonical_->get_base_node(node);

    auto it = node_to_cols_.find(node);

    // if the node hasn't been seen before, or if its annotations haven't
    // been fetched, return nothing
    if (it == node_to_cols_.end() || it->second == nannot)
        return ret_val;

    ret_val.first = &column_sets_.data()[it->second];

    if (has_coordinates()) {
        assert(static_cast<size_t>(it - node_to_cols_.begin()) < label_coords_.size());
        ret_val.second = std::shared_ptr<const CoordinateSet>{
            std::shared_ptr<const CoordinateSet>{},
            &label_coords_[it - node_to_cols_.begin()]
        };

        if (!skip_unfetched && ret_val.second->empty()) {
            node_index base_node = node;

            // TODO: avoid this get_node_sequence call
            if (graph_.get_mode() == DeBruijnGraph::CANONICAL && !canonical_)
                base_node = map_to_nodes(graph_, graph_.get_node_sequence(node))[0];

            Row row = AnnotatedDBG::graph_to_anno_index(base_node);
            auto fetch = multi_int_->get_row_tuples(row);
            std::sort(fetch.begin(), fetch.end());
            CoordinateSet coord_set;
            assert(fetch.size() == ret_val.first->size());
            for (size_t i = 0; i < fetch.size(); ++i) {
                auto &[column, coords] = fetch[i];
                assert(column == (*ret_val.first)[i]);
                coord_set.emplace_back(coords.begin(), coords.end());
            }
            ret_val.second = std::make_shared<const CoordinateSet>(std::move(coord_set));
        }
    }

    return ret_val;
}

} // namespace align
} // namespace graph
} // namespace mtg
