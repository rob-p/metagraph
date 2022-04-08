#ifndef __TUPLE_ROW_DIFF_HPP__
#define __TUPLE_ROW_DIFF_HPP__

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
#include "graph/representation/succinct/boss.hpp"
#include "graph/representation/succinct/dbg_succinct.hpp"
#include "annotation/binary_matrix/row_diff/row_diff.hpp"
#include "annotation/int_matrix/base/int_matrix.hpp"


namespace mtg {
namespace annot {
namespace matrix {

template <class BaseMatrix>
class TupleRowDiff : public binmat::IRowDiff, public MultiIntMatrix {
  public:
    static_assert(std::is_convertible<BaseMatrix*, MultiIntMatrix*>::value);
    static const int SHIFT = 1; // coordinates increase by 1 at each edge

    TupleRowDiff() {}

    TupleRowDiff(const graph::DBGSuccinct *graph, BaseMatrix&& diff)
        : diffs_(std::move(diff)) { graph_ = graph; }

    bool get(Row i, Column j) const override;
    std::vector<Row> get_column(Column j) const override;
    std::vector<SetBitPositions> get_rows(const std::vector<Row> &row_ids) const override;
    RowTuples get_row_tuples(Row i) const override;
    std::vector<RowTuples> get_row_tuples(const std::vector<Row> &rows) const override;
    std::vector<RowTuples> get_row_tuple_diffs(const std::vector<Row> &rows, const RowTuples *first_tuple = nullptr) const override;

    uint64_t num_columns() const override { return diffs_.num_columns(); }
    uint64_t num_relations() const override { return diffs_.num_relations(); }
    uint64_t num_attributes() const override { return diffs_.num_attributes(); }
    uint64_t num_rows() const override { return diffs_.num_rows(); }

    bool load(std::istream &in) override;
    void serialize(std::ostream &out) const override;

    const BaseMatrix& diffs() const { return diffs_; }
    BaseMatrix& diffs() { return diffs_; }

  private:
    static void decode_diffs(RowTuples *diffs);
    static void add_diff(const RowTuples &diff, RowTuples *row);

    BaseMatrix diffs_;
};


template <class BaseMatrix>
bool TupleRowDiff<BaseMatrix>::get(Row i, Column j) const {
    SetBitPositions set_bits = get_row(i);
    auto v = std::lower_bound(set_bits.begin(), set_bits.end(), j);
    return v != set_bits.end() && *v == j;
}

template <class BaseMatrix>
std::vector<MultiIntMatrix::Row> TupleRowDiff<BaseMatrix>::get_column(Column j) const {
    assert(graph_ && "graph must be loaded");
    assert(anchor_.size() == diffs_.num_rows() && "anchors must be loaded");

    const graph::boss::BOSS &boss = graph_->get_boss();
    assert(!fork_succ_.size() || fork_succ_.size() == boss.get_last().size());

    // TODO: implement a more efficient algorithm
    std::vector<Row> result;
    for (Row i = 0; i < num_rows(); ++i) {
        auto edge = graph_->kmer_to_boss_index(
            graph::AnnotatedSequenceGraph::anno_to_graph_index(i)
        );

        if (boss.get_W(edge) && get(i, j))
            result.push_back(i);
    }
    return result;
}

template <class BaseMatrix>
MultiIntMatrix::RowTuples TupleRowDiff<BaseMatrix>::get_row_tuples(Row row) const {
    return get_row_tuples(std::vector<Row>{ row })[0];
}

template <class BaseMatrix>
std::vector<MultiIntMatrix::RowTuples>
TupleRowDiff<BaseMatrix>::get_row_tuples(const std::vector<Row> &row_ids) const {
    assert(graph_ && "graph must be loaded");
    assert(anchor_.size() == diffs_.num_rows() && "anchors must be loaded");
    assert(!fork_succ_.size() || fork_succ_.size() == graph_->get_boss().get_last().size());

    // get row-diff paths
    auto [rd_ids, rd_paths_trunc] = get_rd_ids(row_ids);

    std::vector<RowTuples> rd_rows = diffs_.get_row_tuples(rd_ids);
    for (auto &row : rd_rows) {
        decode_diffs(&row);
    }

    rd_ids = std::vector<Row>();

    // reconstruct annotation rows from row-diff
    std::vector<RowTuples> rows(row_ids.size());

    for (size_t i = 0; i < row_ids.size(); ++i) {
        RowTuples &result = rows[i];

        auto it = rd_paths_trunc[i].rbegin();
        std::sort(rd_rows[*it].begin(), rd_rows[*it].end());
        result = rd_rows[*it];
        // propagate back and reconstruct full annotations for predecessors
        for (++it ; it != rd_paths_trunc[i].rend(); ++it) {
            std::sort(rd_rows[*it].begin(), rd_rows[*it].end());
            add_diff(rd_rows[*it], &result);
            // replace diff row with full reconstructed annotation
            rd_rows[*it] = result;
        }
        assert(std::all_of(result.begin(), result.end(),
                           [](auto &p) { return p.second.size(); }));
    }

    return rows;
}

template <class BaseMatrix>
std::vector<MultiIntMatrix::RowTuples>
TupleRowDiff<BaseMatrix>::get_row_tuple_diffs(const std::vector<Row> &row_ids, const RowTuples *first_tuple) const {
    if (row_ids.empty())
        return {};

    if (row_ids.size() == 1) {
        if (first_tuple)
            return { *first_tuple };

        return get_row_tuples(row_ids);
    }

    assert(graph_ && "graph must be loaded");
    assert(anchor_.size() == diffs_.num_rows() && "anchors must be loaded");
    assert(!fork_succ_.size() || fork_succ_.size() == graph_->get_boss().get_last().size());

    // get row-diff paths
    // diff rows annotating nodes along the row-diff paths
    std::vector<Row> rd_ids;
    rd_ids.reserve(row_ids.size() * binmat::RD_PATH_RESERVE_SIZE);
    if (first_tuple) {
        rd_ids.push_back(row_ids[0]);
    }

    // Truncated row-diff paths, indexes to |rd_rows|.
    // The last index in each path points to an anchor or to a row which had
    // been reached before, and thus, will be reconstructed before this one.
    std::vector<std::vector<size_t>> rd_paths_trunc(row_ids.size());

    const graph::boss::BOSS &boss = graph_->get_boss();
    const bit_vector &rd_succ = fork_succ_.size() ? fork_succ_ : boss.get_last();

    {
        // map row index to its index in |rd_rows|
        VectorMap<Row, size_t> node_to_rd;
        node_to_rd.reserve(row_ids.size() * binmat::RD_PATH_RESERVE_SIZE);
        size_t i = 0;
        if (first_tuple) {
            node_to_rd[row_ids[0]] = 0;
            rd_paths_trunc[0].assign(1, 0);
            ++i;
        }

        for ( ; i < row_ids.size(); ++i) {
            Row row = row_ids[i];

            graph::boss::BOSS::edge_index boss_edge = graph_->kmer_to_boss_index(
                    graph::AnnotatedSequenceGraph::anno_to_graph_index(row));

            while (true) {
                row = graph::AnnotatedSequenceGraph::graph_to_anno_index(
                        graph_->boss_to_kmer_index(boss_edge));

                auto [it, is_new] = node_to_rd.try_emplace(row, rd_ids.size());
                rd_paths_trunc[i].push_back(it.value());

                // If a node had been reached before, we interrupt the diff path.
                // The annotation for that node will have been reconstructed earlier
                // than for other nodes in this path as well. Thus, we will start
                // reconstruction from that node and don't need its successors.
                if (!is_new)
                    break;

                rd_ids.push_back(row);

                if (anchor_[row])
                    break;

                boss_edge = boss.row_diff_successor(boss_edge, rd_succ);
            }
        }
    }

    std::vector<bool> next_is_succ(row_ids.size(), false);
    for (size_t i = 0; i < row_ids.size() - 1; ++i) {
        graph::boss::BOSS::edge_index boss_edge = graph_->kmer_to_boss_index(
                graph::AnnotatedSequenceGraph::anno_to_graph_index(row_ids[i]));
        graph::boss::BOSS::edge_index next_boss_edge = graph_->kmer_to_boss_index(
                graph::AnnotatedSequenceGraph::anno_to_graph_index(row_ids[i + 1]));
        next_is_succ[i] = (boss.row_diff_successor(boss_edge, rd_succ) == next_boss_edge);
    }

    if (first_tuple)
        rd_ids.erase(rd_ids.begin(), rd_ids.begin() + 1);

    std::vector<RowTuples> rd_rows = diffs_.get_row_tuples(rd_ids);
    for (auto &row : rd_rows) {
        decode_diffs(&row);
    }

    rd_ids = std::vector<Row>();

    if (first_tuple)
        rd_rows.insert(rd_rows.begin(), *first_tuple);

    // reconstruct annotation rows from row-diff
    std::vector<RowTuples> rows(row_ids.size());

    for (size_t i = 0; i < row_ids.size(); ++i) {
        RowTuples &result = rows[i];

        auto it = rd_paths_trunc[i].rbegin();
        std::sort(rd_rows[*it].begin(), rd_rows[*it].end());
        result = rd_rows[*it];
        // propagate back and reconstruct full annotations for predecessors
        for (++it ; it != rd_paths_trunc[i].rend(); ++it) {
            std::sort(rd_rows[*it].begin(), rd_rows[*it].end());
            add_diff(rd_rows[*it], &result);
            // replace diff row with full reconstructed annotation
            rd_rows[*it] = result;
        }
        assert(std::all_of(result.begin(), result.end(),
                           [](auto &p) { return p.second.size(); }));
        assert(result == get_row_tuples(row_ids[i]));
        if (i > 1) {
            RowTuples result_diff;
            auto temp = rows[i - 2];
            for (auto &[j, tuple] : temp) {
                assert(std::is_sorted(tuple.begin(), tuple.end()));
                for (uint64_t &c : tuple) {
                    c += SHIFT;
                }
            }
            auto it = rows[i - 1].begin();
            auto it2 = temp.begin();
            while (it != rows[i - 1].end() && it2 != temp.end()) {
                if (it->first < it2->first) {
                    result_diff.push_back(*it);
                    ++it;
                } else if (it->first > it2->first) {
                    result_diff.push_back(*it2);
                    ++it2;
                } else {
                    result_diff.emplace_back(it->first, Tuple{});
                    std::set_symmetric_difference(it->second.begin(), it->second.end(),
                                                  it2->second.begin(), it2->second.end(),
                                                  std::back_inserter(result_diff.back().second));
                    if (result_diff.back().second.empty())
                        result_diff.pop_back();

                    ++it;
                    ++it2;
                }
            }
            std::copy(it, rows[i - 1].end(), std::back_inserter(result_diff));
            std::copy(it2, temp.end(), std::back_inserter(result_diff));

            std::swap(rows[i - 1], result_diff);
        }
    }

    if (rows.size() > 1) {
        size_t i = rows.size();
        RowTuples result_diff;
        auto temp = rows[i - 2];
        for (auto &[j, tuple] : temp) {
            assert(std::is_sorted(tuple.begin(), tuple.end()));
            for (uint64_t &c : tuple) {
                c += SHIFT;
            }
        }
        auto it = rows[i - 1].begin();
        auto it2 = temp.begin();
        while (it != rows[i - 1].end() && it2 != temp.end()) {
            if (it->first < it2->first) {
                result_diff.push_back(*it);
                ++it;
            } else if (it->first > it2->first) {
                result_diff.push_back(*it2);
                ++it2;
            } else {
                result_diff.emplace_back(it->first, Tuple{});
                std::set_symmetric_difference(it->second.begin(), it->second.end(),
                                              it2->second.begin(), it2->second.end(),
                                              std::back_inserter(result_diff.back().second));
                ++it;
                ++it2;
            }
        }
        std::copy(it, rows[i - 1].end(), std::back_inserter(result_diff));
        std::copy(it2, temp.end(), std::back_inserter(result_diff));

        std::swap(rows[i - 1], result_diff);
    }

    assert(rows[0] == get_row_tuples(row_ids[0]));

    return rows;
}

template <class BaseMatrix>
std::vector<MultiIntMatrix::SetBitPositions>
TupleRowDiff<BaseMatrix>::get_rows(const std::vector<Row> &row_ids) const {
    auto row_tuples = get_row_tuples(row_ids);
    std::vector<SetBitPositions> rows;
    rows.reserve(row_tuples.size());
    for (auto &row_tuple : row_tuples) {
        rows.emplace_back();
        rows.back().reserve(row_tuple.size());
        for (auto &[c, tuple] : row_tuple) {
            rows.back().push_back(c);
        }
    }

    return rows;
}

template <class BaseMatrix>
bool TupleRowDiff<BaseMatrix>::load(std::istream &in) {
    std::string version(4, '\0');
    in.read(version.data(), 4);
    return anchor_.load(in) && fork_succ_.load(in) && diffs_.load(in);
}

template <class BaseMatrix>
void TupleRowDiff<BaseMatrix>::serialize(std::ostream &out) const {
    out.write("v2.0", 4);
    anchor_.serialize(out);
    fork_succ_.serialize(out);
    diffs_.serialize(out);
}

template <class BaseMatrix>
void TupleRowDiff<BaseMatrix>::decode_diffs(RowTuples *diffs) {
    std::ignore = diffs;
    // no encoding
}

template <class BaseMatrix>
void TupleRowDiff<BaseMatrix>::add_diff(const RowTuples &diff, RowTuples *row) {
    assert(std::is_sorted(row->begin(), row->end()));
    assert(std::is_sorted(diff.begin(), diff.end()));

    if (diff.size()) {
        RowTuples result;
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
                if (it2->second.size()) {
                    result.emplace_back(it->first, Tuple{});
                    std::set_symmetric_difference(it->second.begin(), it->second.end(),
                                                  it2->second.begin(), it2->second.end(),
                                                  std::back_inserter(result.back().second));
                }
                ++it;
                ++it2;
            }
        }
        std::copy(it, row->end(), std::back_inserter(result));
        std::copy(it2, diff.end(), std::back_inserter(result));

        row->swap(result);
    }

    assert(std::is_sorted(row->begin(), row->end()));
    for (auto &[j, tuple] : *row) {
        assert(std::is_sorted(tuple.begin(), tuple.end()));
        for (uint64_t &c : tuple) {
            assert(c >= SHIFT);
            c -= SHIFT;
        }
    }
}

} // namespace matrix
} // namespace annot
} // namespace mtg

#endif // __TUPLE_ROW_DIFF_HPP__
