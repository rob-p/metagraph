#ifndef __INT_ROW_DIFF_HPP__
#define __INT_ROW_DIFF_HPP__

#include <algorithm>
#include <iostream>
#include <cassert>
#include <string>
#include <vector>

#include "common/vectors/bit_vector_adaptive.hpp"
#include "common/vector_map.hpp"
#include "common/vector.hpp"
#include "common/logger.hpp"
#include "common/utils/template_utils.hpp"
#include "graph/annotated_dbg.hpp"
#include "graph/representation/succinct/dbg_succinct.hpp"
#include "annotation/binary_matrix/row_diff/row_diff.hpp"
#include "annotation/int_matrix/base/int_matrix.hpp"


namespace mtg {
namespace annot {
namespace matrix {

const size_t RD_PATH_RESERVE_SIZE = 2;


/**
 * Convert deltas to positive integer for enable compression:
 *      0  -> X (not allowed, zero diffs must be skipped)
 *      1  -> 0
 *      -1 -> 1
 *      2  -> 2
 *      -2 -> 3
 *      ...
 */
inline uint64_t encode_diff(int64_t x) {
    assert(x);
    return (std::abs(x) - 1) * 2 + (x < 0);
}

inline int64_t decode_diff(uint64_t c) {
    return !(c & 1) ? c / 2 + 1 : -((c + 1) / 2);
}

template <class BaseMatrix>
class IntRowDiff : public binmat::IRowDiff, public IntMatrix {
  public:
    using anchor_bv_type = bit_vector_small;
    using fork_succ_bv_type = bit_vector_small;
    static_assert(std::is_convertible<BaseMatrix*, IntMatrix*>::value);

    IntRowDiff() {}

    IntRowDiff(const graph::DBGSuccinct *graph, BaseMatrix&& diff)
        : diffs_(std::move(diff)) { graph_ = graph; }

    bool get(Row i, Column j) const override;
    std::vector<Row> get_column(Column j) const override;
    SetBitPositions get_row(Row i) const override;
    std::vector<SetBitPositions> get_rows(const std::vector<Row> &rows) const override;
    // query integer values
    RowValues get_row_values(Row i) const override;
    std::vector<RowValues> get_row_values(const std::vector<Row> &rows) const override;

    uint64_t num_columns() const override { return diffs_.num_columns(); }
    uint64_t num_relations() const override { return diffs_.num_relations(); }
    uint64_t num_rows() const override { return diffs_.num_rows(); }

    bool load(std::istream &in) override;
    void serialize(std::ostream &out) const override;

    void load_fork_succ(const std::string &filename);
    void load_anchor(const std::string &filename);

    const anchor_bv_type& anchor() const { return anchor_; }
    const BaseMatrix& diffs() const { return diffs_; }
    BaseMatrix& diffs() { return diffs_; }

  private:
    static void decode_diffs(RowValues *diffs);
    static void add_diff(const RowValues &diff, RowValues *row);

    BaseMatrix diffs_;
    anchor_bv_type anchor_;
    fork_succ_bv_type fork_succ_;
};


template <class BaseMatrix>
bool IntRowDiff<BaseMatrix>::get(Row i, Column j) const {
    SetBitPositions set_bits = get_row(i);
    auto v = std::lower_bound(set_bits.begin(), set_bits.end(), j);
    return v != set_bits.end() && *v == j;
}

template <class BaseMatrix>
std::vector<IntMatrix::Row> IntRowDiff<BaseMatrix>::get_column(Column j) const {
    assert(graph_ && "graph must be loaded");
    assert(anchor_.size() == diffs_.num_rows() && "anchors must be loaded");
    assert(!fork_succ_.size() || fork_succ_.size() == graph_->num_nodes() + 1);

    // TODO: implement a more efficient algorithm
    std::vector<Row> result;
    for (Row i = 0; i < num_rows(); ++i) {
        if (get(i, j))
            result.push_back(i);
    }
    return result;
}

template <class BaseMatrix>
IntMatrix::SetBitPositions IntRowDiff<BaseMatrix>::get_row(Row i) const {
    RowValues row = get_row_values(i);
    SetBitPositions result(row.size());
    for (size_t k = 0; k < row.size(); ++k) {
        result[k] = row[k].first;
    }
    return result;
}

template <class BaseMatrix>
IntMatrix::RowValues IntRowDiff<BaseMatrix>::get_row_values(Row row) const {
    return get_row_values(std::vector<Row>{ row })[0];
}

template <class BaseMatrix>
std::vector<IntMatrix::SetBitPositions>
IntRowDiff<BaseMatrix>::get_rows(const std::vector<Row> &row_ids) const {
    std::vector<SetBitPositions> result;
    result.reserve(row_ids.size());

    for (auto&& row : get_row_values(row_ids)) {
        result.emplace_back(row.size());
        for (size_t k = 0; k < row.size(); ++k) {
            result.back()[k] = row[k].first;
        }
        row = RowValues();
    }

    return result;
}

template <class BaseMatrix>
std::vector<IntMatrix::RowValues>
IntRowDiff<BaseMatrix>::get_row_values(const std::vector<Row> &row_ids) const {
    assert(graph_ && "graph must be loaded");
    assert(anchor_.size() == diffs_.num_rows() && "anchors must be loaded");
    assert(!fork_succ_.size() || fork_succ_.size() == graph_->num_nodes() + 1);

    // diff rows annotating nodes along the row-diff paths
    std::vector<Row> rd_ids;
    rd_ids.reserve(row_ids.size() * RD_PATH_RESERVE_SIZE);

    // map row index to its index in |rd_rows|
    VectorMap<Row, size_t> node_to_rd;
    node_to_rd.reserve(row_ids.size() * RD_PATH_RESERVE_SIZE);

    // Truncated row-diff paths, indexes to |rd_rows|.
    // The last index in each path points to an anchor or to a row which had
    // been reached before, and thus, will be reconstructed before this one.
    std::vector<std::vector<std::pair<size_t, size_t>>> rd_paths_trunc(row_ids.size());

    for (size_t i = 0; i < row_ids.size(); ++i) {
        std::vector<std::pair<size_t, size_t>> &rd_path = rd_paths_trunc[i];

        std::vector<size_t> path;
        Vector<std::pair<size_t, Row>> queue;
        queue.emplace_back(0, row_ids[i]);

        while (queue.size()) {
            size_t depth = queue.back().first;
            Row row = queue.back().second;
            queue.pop_back();
            while (depth < path.size()) {
                assert(path.size() > 1);
                rd_path.emplace_back(*(path.rbegin() + 1), *path.rbegin());
                path.pop_back();
            }
            auto [it, is_new] = node_to_rd.try_emplace(row, rd_ids.size());
            path.push_back(it.value());
            // If a node had been reached before, we interrupt the diff path.
            // The annotation for that node will have been reconstructed earlier
            // than for other nodes in this path as well. Thus, we will start
            // reconstruction from that node and don't need its successors.
            if (!is_new)
                continue;

            rd_ids.push_back(row);

            if (anchor_[row])
                continue;

            auto node = graph::AnnotatedSequenceGraph::anno_to_graph_index(row);
            graph_->call_row_diff_successors(node, fork_succ_, [&](auto succ) {
                queue.emplace_back(depth + 1, graph::AnnotatedSequenceGraph::graph_to_anno_index(succ));
            });
        }

        while (path.size() > 1) {
            rd_path.emplace_back(*(path.rbegin() + 1), *path.rbegin());
            path.pop_back();
        }
        assert(path.size());
        rd_path.emplace_back(-1, path[0]);
    }

    node_to_rd = VectorMap<Row, size_t>();

    std::vector<RowValues> rd_rows = diffs_.get_row_values(rd_ids);
    for (auto &row : rd_rows) {
        decode_diffs(&row);
        std::sort(row.begin(), row.end());
    }

    rd_ids = std::vector<Row>();

    // reconstruct annotation rows from row-diff
    std::vector<RowValues> rows(row_ids.size());

    for (size_t i = 0; i < row_ids.size(); ++i) {
        const auto &rd_path = rd_paths_trunc[i];
        // propagate back and reconstruct full annotations for predecessors
        for (size_t j = 0; j + 1 < rd_path.size(); ++j) {
            auto [node, succ] = rd_path[j];
            // reconstruct annotation by adding the diff (full succ + diff)
            add_diff(rd_rows[succ], &rd_rows[node]);
        }
        rows[i] = rd_rows[rd_path.back().second];
        assert(std::all_of(rows[i].begin(), rows[i].end(),
                           [](auto &p) { return p.second; }));
        assert(std::all_of(rows[i].begin(), rows[i].end(),
                           [](auto &p) { return (int64_t)p.second > 0; }));
    }

    return rows;
}

template <class BaseMatrix>
bool IntRowDiff<BaseMatrix>::load(std::istream &in) {
    std::string version(4, '\0');
    in.read(version.data(), 4);
    return anchor_.load(in) && fork_succ_.load(in) && diffs_.load(in);
}

template <class BaseMatrix>
void IntRowDiff<BaseMatrix>::serialize(std::ostream &out) const {
    out.write("v2.0", 4);
    anchor_.serialize(out);
    fork_succ_.serialize(out);
    diffs_.serialize(out);
}

template <class BaseMatrix>
void IntRowDiff<BaseMatrix>::decode_diffs(RowValues *diffs) {
    for (auto &[j, value] : *diffs) {
        value = decode_diff(value);
    }
}

template <class BaseMatrix>
void IntRowDiff<BaseMatrix>::add_diff(const RowValues &diff, RowValues *row) {
    assert(std::is_sorted(row->begin(), row->end()));
    assert(std::is_sorted(diff.begin(), diff.end()));

    if (diff.empty())
        return;

    RowValues result;
    result.reserve(row->size() + diff.size());

    auto it = row->begin();
    auto it2 = diff.begin();
    while (it != row->end() && it2 != diff.end()) {
        if (it->first < it2->first) {
            result.push_back(*it);
            ++it;
        } else if (it->first > it2->first) {
            result.push_back(*it2);
            ++it2;
        } else {
            if (uint64_t sum = it->second + it2->second)
                result.emplace_back(it->first, sum);
            ++it;
            ++it2;
        }
    }
    std::copy(it, row->end(), std::back_inserter(result));
    std::copy(it2, diff.end(), std::back_inserter(result));

    row->swap(result);
}

template <class BaseMatrix>
void IntRowDiff<BaseMatrix>::load_anchor(const std::string &filename) {
    if (!std::filesystem::exists(filename)) {
        common::logger->error("Can't read anchor file: {}", filename);
        std::exit(1);
    }
    std::ifstream in(filename, ios::binary);
    if (!in.good()) {
        common::logger->error("Could not open anchor file {}", filename);
        std::exit(1);
    }
    anchor_.load(in);
}

template <class BaseMatrix>
void IntRowDiff<BaseMatrix>::load_fork_succ(const std::string &filename) {
    if (!std::filesystem::exists(filename)) {
        common::logger->error("Can't read fork successor file: {}", filename);
        std::exit(1);
    }
    std::ifstream in(filename, ios::binary);
    if (!in.good()) {
        common::logger->error("Could not open fork successor file {}", filename);
        std::exit(1);
    }
    fork_succ_.load(in);
}

} // namespace matrix
} // namespace annot
} // namespace mtg

#endif // __INT_ROW_DIFF_HPP__
