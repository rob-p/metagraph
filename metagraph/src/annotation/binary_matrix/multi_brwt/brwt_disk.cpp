#include "brwt_disk.hpp"

#include <queue>
#include <numeric>

#include "common/algorithms.hpp"
#include "common/serialization.hpp"
#include "common/utils/template_utils.hpp"


namespace mtg {
namespace annot {
namespace binmat {

bool BRWT_Disk::get(Row row, Column column) const {
    disk_manager->notify_get_called(); // mkokot, TODO: for debuging, remove
    assert(row < num_rows());
    assert(column < num_columns());

    // terminate if the index bit is unset
    //if (!(*nonzero_rows_)[row])
    auto bv = get_bit_vector();
    if (!(*bv)[row])
        return false;

    // return true if this is a leaf
    if (!child_nodes_.size())
        return true;

    auto child_node = assignments_.group(column);
    return child_nodes_[child_node]->get(bv->rank1(row) - 1,
                                         assignments_.rank(column));
}

BRWT_Disk::SetBitPositions BRWT_Disk::get_row(Row row) const {
    disk_manager->notify_get_row_called(); // mkokot, TODO: for debuging, remove
    assert(row < num_rows());
    
    auto bv = get_bit_vector();
    // check if the row is empty
    if (!(*bv)[row])
        return {};

    // check whether it is a leaf
    if (!child_nodes_.size()) {
        assert(assignments_.size() == 1);
        // the bit is set
        return { 0 };
    }

    // check all child nodes
    SetBitPositions row_set_bits;
    uint64_t index_in_child = bv->rank1(row) - 1;

    for (size_t i = 0; i < child_nodes_.size(); ++i) {
        const auto &child = *child_nodes_[i];

        for (auto col_id : child.get_row(index_in_child)) {
            row_set_bits.push_back(assignments_.get(i, col_id));
        }
    }
    return row_set_bits;
}

Vector<std::pair<BRWT_Disk::Column, uint64_t>> BRWT_Disk::get_column_ranks(Row i) const {
    assert(i < num_rows());

    auto bv = get_bit_vector();
    // check if the row is empty
    uint64_t rank = bv->conditional_rank1(i);
    if (!rank)
        return {};

    // check whether it is a leaf
    if (!child_nodes_.size()) {
        assert(assignments_.size() == 1);
        // the bit is set
        return {{ 0, rank }};
    }

    // check all child nodes
    Vector<std::pair<BRWT_Disk::Column, uint64_t>> row;
    uint64_t index_in_child = rank - 1;

    for (size_t k = 0; k < child_nodes_.size(); ++k) {
        const auto &child = *child_nodes_[k];

        for (auto [col_id, rank] : child.get_column_ranks(index_in_child)) {
            row.emplace_back(assignments_.get(k, col_id), rank);
        }
    }
    return row;
}

std::vector<BRWT_Disk::SetBitPositions>
BRWT_Disk::get_rows(const std::vector<Row> &row_ids) const {
    disk_manager->notify_get_rows_called(); // mkokot, TODO: for debuging, remove
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

std::vector<BRWT_Disk::Column> BRWT_Disk::slice_rows(const std::vector<Row> &row_ids) const {
    return slice_rows<Column>(row_ids);
}

// If T = Column
//      return positions of set bits.
// If T = std::pair<Column, uint64_t>
//      return positions of set bits with their column ranks.
template <typename T>
std::vector<T> BRWT_Disk::slice_rows(const std::vector<Row> &row_ids) const {
    // mkokot, TODO: remove, time measurements with strange trick to detect top recurrence level
    //it will NOT work for multithreaded
    static size_t reku_depth = 0;
    std::chrono::high_resolution_clock::time_point start;
    if (++reku_depth == 1)
        start = std::chrono::high_resolution_clock::now();        
    //    
    nonzero_rows_->slice_rows_cals++;
    std::vector<T> slice;
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
                if (uint64_t rank = get_bit_vector()->conditional_rank1(i)) {
                    // only a single column is stored in leaves
                    slice.emplace_back(0, rank);
                }
            } else {
                if ((*get_bit_vector())[i]) {
                    // only a single column is stored in leaves
                    slice.push_back(0);
                }
            }
            slice.push_back(delim);
        }
        count_as_new_access = true;
        if (--reku_depth == 0)
            mtg::common::logger->trace("slice_rows time: {}", std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count());        
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
        auto bv = get_bit_vector();
        if (i + 4 < row_ids.size()
                && row_ids[i + 4] < global_offset + 64
                && row_ids[i + 4] >= global_offset
                && global_offset + 64 <= bv->size()) {
            // get the word
            uint64_t word = bv->get_int(global_offset, 64);
            uint64_t rank = -1ULL;

            do {
                // check index
                uint8_t offset = row_ids[i] - global_offset;
                if (word & (1ULL << offset)) {
                    if (rank == -1ULL)
                        rank = global_offset > 0
                                ? bv->rank1(global_offset - 1)
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
            if (uint64_t rank = bv->conditional_rank1(global_offset)) {
                // map index from parent's to children's coordinate system
                child_row_ids.push_back(rank - 1);
                skip_row[i] = false;
            }
        }
    }

    if (!child_row_ids.size())
    {
        count_as_new_access = true;
        if (--reku_depth == 0)
            mtg::common::logger->trace("slice_rows time: {}", std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count());
        return std::vector<T>(row_ids.size(), delim);
    }
        

    // TODO: query by columns and merge them in the very end to avoid remapping
    //       the same column indexes many times when propagating to the root.
    // TODO: implement a cache efficient method for merging the columns.

    // query all children subtrees and get relations from them
    std::vector<std::vector<T>> child_slices(child_nodes_.size());
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

    count_as_new_access = true;
    if (--reku_depth == 0)
        mtg::common::logger->trace("slice_rows time: {}", std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count());
    return slice;
}

std::vector<Vector<std::pair<BRWT_Disk::Column, uint64_t>>>
BRWT_Disk::get_column_ranks(const std::vector<Row> &row_ids) const {
    std::vector<Vector<std::pair<Column, uint64_t>>> rows(row_ids.size());

    std::vector<std::pair<Column, uint64_t>> slice
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

std::vector<BRWT_Disk::Row> BRWT_Disk::get_column(Column column) const {
    assert(column < num_columns());

    auto bv = get_bit_vector();
    auto num_nonzero_rows = bv->num_set_bits();

    // check if the column is empty
    if (!num_nonzero_rows)
        return {};

    // check whether it is a leaf
    if (!child_nodes_.size()) {
        // return the index column
        std::vector<BRWT_Disk::Row> result;
        result.reserve(num_nonzero_rows);
        bv->call_ones([&](auto i) { result.push_back(i); });
        return result;
    }

    auto child_node = assignments_.group(column);
    auto rows = child_nodes_[child_node]->get_column(assignments_.rank(column));

    // check if we need to update the row indexes
    if (num_nonzero_rows == bv->size())
        return rows;

    // shift indexes
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i] = bv->select1(rows[i] + 1);
    }
    return rows;
}


bool BRWT_Disk::load_impl(std::istream &in, NodeDepth depth)
{
    if (!in.good())
        return false;

    try {
        if (!assignments_.load(in))
            return false;

        auto start_pos = in.tellg();
        std::unique_ptr<bit_vector> nonzero_rows_tmp = std::make_unique<bit_vector_smallrank>(); // mkokot TODO: find a better way to determine the offset than reading

        if (!nonzero_rows_tmp->load(in))
            return false;

        nonzero_rows_tmp.reset();

        auto end_pos = in.tellg();
        auto in_file_size = end_pos - start_pos;
        
        nonzero_rows_ = std::make_unique<NonZeroRows>(start_pos, in_file_size, depth);

        size_t num_child_nodes = load_number(in);
        child_nodes_.clear();
        child_nodes_.reserve(num_child_nodes);
        for (size_t i = 0; i < num_child_nodes; ++i) {
            child_nodes_.emplace_back(new BRWT_Disk(disk_manager));
            if (!child_nodes_.back()->load_impl(in, depth + 1))
                return false;
        }
        return !child_nodes_.size()
                    || child_nodes_.size() == assignments_.num_groups();
    } catch (...) {
        return false;
    }
}

bool BRWT_Disk::load(std::istream &in) {    
    
    auto ptr = dynamic_cast<IfstreamWithNameAndOffset*>(&in);
    if (!ptr) {                
        logger->error("BRWT_Disk::load requires IfstreamWithNameAndOffset object");
        return false;
    }   

    //auto brwt_max_anno_mem = mtg::cli::GlobalConfigAccess::Inst().Get()->brwt_max_anno_mem;
    assert(brwt_max_anno_mem);
    std::cerr << "Maximum memory for brwt is " << brwt_max_anno_mem << "\n";
    disk_manager = std::make_shared<BRWT_DiskManager>(brwt_max_anno_mem, ptr->GetFName()); // mkokot TODO: consider how to set the allowed memory

    auto time_start = std::chrono::high_resolution_clock::now(); // mkokot, TODO: remove
    if (!load_impl(in, 0))
        return false;

    auto time = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - time_start).count(); // mkokot, TODO: remove
    std::cerr << "BRWT_Disk::load_impl time: " << time << "s\n";

    // mkokot TODO: this is for debuging and logging, consider removing
    std::queue<const BRWT_Disk*> nodes_queue;
    std::map<size_t, size_t> nodes_per_depth;
    nodes_queue.push(this);
    nodes_per_depth[0] = 1;

    uint32_t depth{};
    while (!nodes_queue.empty()) {
        const auto &node = *nodes_queue.front();

        disk_manager->register_node(depth, node.nonzero_rows_.get());

        nodes_per_depth[depth+1] += node.child_nodes_.size();
        if (!--nodes_per_depth[depth])
            ++depth;

        for (const auto &child_node : node.child_nodes_) {
            const auto *brwt_node_ptr = dynamic_cast<const BRWT_Disk*>(child_node.get());
            if (brwt_node_ptr)
                nodes_queue.push(brwt_node_ptr);
        }
        nodes_queue.pop();
    }    
    ////////////////////////////////////////////////////////////////////////////////
    // mkokot, debugowe to check what is going on with tot_req_size
    //disk_manager->printStats();
    //exit(1);
    
    
    return true;
}

void BRWT_Disk::serialize(std::ostream &out) const {
    if (!out.good())
        throw std::ofstream::failure("Error when dumping BRWT_Disk");

    assignments_.serialize(out);

    assert(!child_nodes_.size()
                || child_nodes_.size() == assignments_.num_groups());

    get_bit_vector()->serialize(out);

    serialize_number(out, child_nodes_.size());
    for (const auto &child : child_nodes_) {
        child->serialize(out);
    }
}

uint64_t BRWT_Disk::num_relations() const {
    if (!child_nodes_.size())
        return get_bit_vector()->num_set_bits();

    uint64_t num_set_bits = 0;
    for (const auto &submatrix_ptr : child_nodes_) {
        num_set_bits += submatrix_ptr->num_relations();
    }

    return num_set_bits;
}

double BRWT_Disk::avg_arity() const {
    if (!child_nodes_.size())
        return 0;

    uint64_t num_nodes = 0;
    uint64_t total_num_child_nodes = 0;

    BFT([&](const BRWT_Disk &node) {
        if (node.child_nodes_.size()) {
            num_nodes++;
            total_num_child_nodes += node.child_nodes_.size();
        }
    });

    return num_nodes
            ? static_cast<double>(total_num_child_nodes) / num_nodes
            : 0;
}

uint64_t BRWT_Disk::num_nodes() const {
    uint64_t num_nodes = 0;

    BFT([&num_nodes](const BRWT_Disk &) { num_nodes++; });

    return num_nodes;
}

double BRWT_Disk::shrinking_rate() const {
    double rate_sum = 0;
    uint64_t num_nodes = 0;

    BFT([&](const BRWT_Disk &node) {
        if (node.child_nodes_.size()) {
            num_nodes++;
            auto bv = node.get_bit_vector();
            rate_sum += static_cast<double>(bv->num_set_bits())
                            / bv->size();
        }
    });

    return rate_sum / num_nodes;
}

uint64_t BRWT_Disk::total_column_size() const {
    uint64_t total_size = 0;

    BFT([&](const BRWT_Disk &node) {
        total_size += node.get_bit_vector()->size();
    });

    return total_size;
}

uint64_t BRWT_Disk::total_num_set_bits() const {
    uint64_t total_num_set_bits = 0;

    BFT([&](const BRWT_Disk &node) {
        total_num_set_bits += node.get_bit_vector()->num_set_bits();
    });

    return total_num_set_bits;
}

void BRWT_Disk::print_tree_structure(std::ostream &os) const {
    BFT([&os](const BRWT_Disk &node) {
        // print node and its stats
        os << &node << "," << node.get_bit_vector()->size()
                    << "," << node.get_bit_vector()->num_set_bits();
        // print all its children
        for (const auto &child : node.child_nodes_) {
            os << "," << child.get();
        }
        os << std::endl;
    });
}

void BRWT_Disk::BFT(std::function<void(const BRWT_Disk &node)> callback) const {
    std::queue<const BRWT_Disk*> nodes_queue;
    nodes_queue.push(this);

    while (!nodes_queue.empty()) {
        const auto &node = *nodes_queue.front();

        callback(node);

        for (const auto &child_node : node.child_nodes_) {
            const auto *brwt_node_ptr = dynamic_cast<const BRWT_Disk*>(child_node.get());
            if (brwt_node_ptr)
                nodes_queue.push(brwt_node_ptr);
        }
        nodes_queue.pop();
    }
}

} // namespace binmat
} // namespace annot
} // namespace mtg
