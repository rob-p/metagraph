#include "brwt.hpp"

#include <queue>
#include <numeric>

#include "common/algorithms.hpp"
#include "common/serialization.hpp"
#include "common/utils/template_utils.hpp"


namespace mtg {
namespace annot {
namespace matrix {

bool BRWT::get(Row row, Column column) const {
    assert(row < num_rows());
    assert(column < num_columns());

    // if this is a leaf
    if (!child_nodes_.size())
        return (*nonzero_rows_)[row];

    uint64_t rank = nonzero_rows_->conditional_rank1(row);
    // terminate if the index bit is unset
    if (!rank)
        return false;

    auto child_node = assignments_.group(column);
    return child_nodes_[child_node]->get(rank - 1, assignments_.rank(column));
}

Vector<std::pair<BRWT::Column, uint64_t>> BRWT::get_column_ranks(Row i) const {
    assert(i < num_rows());

    // check if the row is empty
    uint64_t rank = nonzero_rows_->conditional_rank1(i);
    if (!rank)
        return {};

    // check whether it is a leaf
    if (!child_nodes_.size()) {
        assert(assignments_.size() == 1);
        // the bit is set
        return {{ 0, rank }};
    }

    // check all child nodes
    Vector<std::pair<BRWT::Column, uint64_t>> row;
    uint64_t index_in_child = rank - 1;

    for (size_t k = 0; k < child_nodes_.size(); ++k) {
        const auto &child = *child_nodes_[k];

        for (auto [col_id, rank] : child.get_column_ranks(index_in_child)) {
            row.emplace_back(assignments_.get(k, col_id), rank);
        }
    }
    return row;
}

std::vector<BRWT::SetBitPositions>
BRWT::get_rows(const std::vector<Row> &row_ids) const {
    std::vector<SetBitPositions> rows(row_ids.size());

    auto slice = slice_rows(row_ids);

    assert(slice.size() >= row_ids.size());

    auto row_begin = slice.begin();

    for (size_t i = 0; i < rows.size(); ++i) {
        // every row in `slice` ends with `-1`
        auto row_end = std::find(row_begin, slice.end(),
                                 std::numeric_limits<Column>::max());
        rows[i].assign(row_begin, row_end);
        row_begin = row_end + 1;
    }

    return rows;
}

BRWT::SetBitPositions BRWT::slice_rows(const std::vector<Row> &row_ids) const {
    return slice_rows<Column>(row_ids);
}

// If T = Column
//      return positions of set bits.
// If T = std::pair<Column, uint64_t>
//      return positions of set bits with their column ranks.
template <typename T>
Vector<T> BRWT::slice_rows(const std::vector<Row> &row_ids) const {
    Vector<T> slice;
    // expect at least one relation per row
    slice.reserve(row_ids.size() * 2);

    T delim;
    if constexpr(utils::is_pair_v<T>) {
        delim = std::make_pair(std::numeric_limits<Column>::max(), 0);
    } else {
        delim = std::numeric_limits<Column>::max();
    }

    // check if this is a leaf
    if (!child_nodes_.size()) {
        assert(assignments_.size() == 1);

        for (Row i : row_ids) {
            assert(i < num_rows());

            if constexpr(utils::is_pair_v<T>) {
                if (uint64_t rank = nonzero_rows_->conditional_rank1(i)) {
                    // only a single column is stored in leaves
                    slice.emplace_back(0, rank);
                }
            } else {
                if ((*nonzero_rows_)[i]) {
                    // only a single column is stored in leaves
                    slice.push_back(0);
                }
            }
            slice.push_back(delim);
        }

        return slice;
    }

    // construct indexing for children and the inverse mapping
    std::vector<Row> child_row_ids;
    child_row_ids.reserve(row_ids.size());

    std::vector<bool> skip_row(row_ids.size(), true);

    for (size_t i = 0; i < row_ids.size(); ++i) {
        assert(row_ids[i] < num_rows());

        uint64_t global_offset = row_ids[i];

        // if next word contains 5 or more positions, query the whole word
        // we assume that get_int is roughly 5 times slower than operator[]
        if (i + 4 < row_ids.size()
                && row_ids[i + 4] < global_offset + 64
                && row_ids[i + 4] >= global_offset
                && global_offset + 64 <= nonzero_rows_->size()) {
            // get the word
            uint64_t word = nonzero_rows_->get_int(global_offset, 64);
            uint64_t rank = -1ULL;

            do {
                // check index
                uint8_t offset = row_ids[i] - global_offset;
                if (word & (1ULL << offset)) {
                    if (rank == -1ULL)
                        rank = global_offset > 0
                                ? nonzero_rows_->rank1(global_offset - 1)
                                : 0;

                    // map index from parent's to children's coordinate system
                    child_row_ids.push_back(rank + sdsl::bits::cnt(word & sdsl::bits::lo_set[offset + 1]) - 1);
                    skip_row[i] = false;
                }
            } while (++i < row_ids.size()
                        && row_ids[i] < global_offset + 64
                        && row_ids[i] >= global_offset);
            --i;

        } else {
            // check index
            if (uint64_t rank = nonzero_rows_->conditional_rank1(global_offset)) {
                // map index from parent's to children's coordinate system
                child_row_ids.push_back(rank - 1);
                skip_row[i] = false;
            }
        }
    }

    if (!child_row_ids.size())
        return Vector<T>(row_ids.size(), delim);

    // TODO: query by columns and merge them in the very end to avoid remapping
    //       the same column indexes many times when propagating to the root.
    // TODO: implement a cache efficient method for merging the columns.

    // query all children subtrees and get relations from them
    std::vector<Vector<T>> child_slices(child_nodes_.size());
    std::vector<const T *> pos(child_nodes_.size());

    for (size_t j = 0; j < child_nodes_.size(); ++j) {
        child_slices[j] = child_nodes_[j]->slice_rows<T>(child_row_ids);
        // transform column indexes

        for (auto &v : child_slices[j]) {
            if (v != delim) {
                auto &col = utils::get_first(v);
                col = assignments_.get(j, col);
            }
        }
        assert(child_slices[j].size() >= child_row_ids.size());
        pos[j] = &child_slices[j].front() - 1;
    }

    for (size_t i = 0; i < row_ids.size(); ++i) {
        if (!skip_row[i]) {
            // merge rows from child submatrices
            for (auto &p : pos) {
                while (*(++p) != delim) {
                    slice.push_back(*p);
                }
            }
        }
        slice.push_back(delim);
    }

    return slice;
}

std::vector<Vector<std::pair<BRWT::Column, uint64_t>>>
BRWT::get_column_ranks(const std::vector<Row> &row_ids) const {
    std::vector<Vector<std::pair<Column, uint64_t>>> rows(row_ids.size());

    Vector<std::pair<Column, uint64_t>> slice
            = slice_rows<std::pair<Column, uint64_t>>(row_ids);

    assert(slice.size() >= row_ids.size());

    auto row_begin = slice.begin();

    for (size_t i = 0; i < rows.size(); ++i) {
        // every row in `slice` ends with `-1`
        auto row_end = row_begin;
        while (row_end->first != std::numeric_limits<Column>::max()) {
            ++row_end;
            assert(row_end != slice.end());
        }
        rows[i].assign(row_begin, row_end);
        row_begin = row_end + 1;
    }

    return rows;
}

std::vector<BRWT::Row> BRWT::get_column(Column column) const {
    assert(column < num_columns());

    auto num_nonzero_rows = nonzero_rows_->num_set_bits();

    // check if the column is empty
    if (!num_nonzero_rows)
        return {};

    // check whether it is a leaf
    if (!child_nodes_.size()) {
        // return the index column
        std::vector<BRWT::Row> result;
        result.reserve(num_nonzero_rows);
        nonzero_rows_->call_ones([&](auto i) { result.push_back(i); });
        return result;
    }

    auto child_node = assignments_.group(column);
    auto rows = child_nodes_[child_node]->get_column(assignments_.rank(column));

    // check if we need to update the row indexes
    if (num_nonzero_rows == nonzero_rows_->size())
        return rows;

    // shift indexes
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i] = nonzero_rows_->select1(rows[i] + 1);
    }
    return rows;
}

bool BRWT::load(std::istream &in) {
    if (!in.good())
        return false;

    try {
        if (!assignments_.load(in))
            return false;

        if (!nonzero_rows_->load(in))
            return false;

        size_t num_child_nodes = load_number(in);
        child_nodes_.clear();
        child_nodes_.reserve(num_child_nodes);
        for (size_t i = 0; i < num_child_nodes; ++i) {
            child_nodes_.emplace_back(new BRWT());
            if (!child_nodes_.back()->load(in))
                return false;
        }
        return !child_nodes_.size()
                    || child_nodes_.size() == assignments_.num_groups();
    } catch (...) {
        return false;
    }
}

void BRWT::serialize(std::ostream &out) const {
    if (!out.good())
        throw std::ofstream::failure("Error when dumping BRWT");

    assignments_.serialize(out);

    assert(!child_nodes_.size()
                || child_nodes_.size() == assignments_.num_groups());

    nonzero_rows_->serialize(out);

    serialize_number(out, child_nodes_.size());
    for (const auto &child : child_nodes_) {
        child->serialize(out);
    }
}

uint64_t BRWT::num_relations() const {
    if (!child_nodes_.size())
        return nonzero_rows_->num_set_bits();

    uint64_t num_set_bits = 0;
    for (const auto &submatrix_ptr : child_nodes_) {
        num_set_bits += submatrix_ptr->num_relations();
    }

    return num_set_bits;
}

double BRWT::avg_arity() const {
    if (!child_nodes_.size())
        return 0;

    uint64_t num_nodes = 0;
    uint64_t total_num_child_nodes = 0;

    BFT([&](const BRWT &node) {
        if (node.child_nodes_.size()) {
            num_nodes++;
            total_num_child_nodes += node.child_nodes_.size();
        }
    });

    return num_nodes
            ? static_cast<double>(total_num_child_nodes) / num_nodes
            : 0;
}

uint64_t BRWT::num_nodes() const {
    uint64_t num_nodes = 0;

    BFT([&num_nodes](const BRWT &) { num_nodes++; });

    return num_nodes;
}

double BRWT::shrinking_rate() const {
    double rate_sum = 0;
    uint64_t num_nodes = 0;

    BFT([&](const BRWT &node) {
        if (node.child_nodes_.size()) {
            num_nodes++;
            rate_sum += static_cast<double>(node.nonzero_rows_->num_set_bits())
                            / node.nonzero_rows_->size();
        }
    });

    return rate_sum / num_nodes;
}

uint64_t BRWT::total_column_size() const {
    uint64_t total_size = 0;

    BFT([&](const BRWT &node) {
        total_size += node.nonzero_rows_->size();
    });

    return total_size;
}

uint64_t BRWT::total_num_set_bits() const {
    uint64_t total_num_set_bits = 0;

    BFT([&](const BRWT &node) {
        total_num_set_bits += node.nonzero_rows_->num_set_bits();
    });

    return total_num_set_bits;
}

void BRWT::print_tree_structure(std::ostream &os) const {
    BFT([&os](const BRWT &node) {
        // print node and its stats
        os << &node << "," << node.nonzero_rows_->size()
                    << "," << node.nonzero_rows_->num_set_bits();
        // print all its children
        for (const auto &child : node.child_nodes_) {
            os << "," << child.get();
        }
        os << std::endl;
    });
}

void BRWT::BFT(std::function<void(const BRWT &node)> callback) const {
    std::queue<const BRWT*> nodes_queue;
    nodes_queue.push(this);

    while (!nodes_queue.empty()) {
        const auto &node = *nodes_queue.front();

        callback(node);

        for (const auto &child_node : node.child_nodes_) {
            const auto *brwt_node_ptr = dynamic_cast<const BRWT*>(child_node.get());
            if (brwt_node_ptr)
                nodes_queue.push(brwt_node_ptr);
        }
        nodes_queue.pop();
    }
}

} // namespace matrix
} // namespace annot
} // namespace mtg
