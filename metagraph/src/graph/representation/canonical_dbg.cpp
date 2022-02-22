#include "canonical_dbg.hpp"

#include "common/seq_tools/reverse_complement.hpp"
#include "kmer/kmer_extractor.hpp"
#include "common/logger.hpp"
#include "graph/graph_extensions/node_first_cache.hpp"
#include "graph/representation/succinct/dbg_succinct.hpp"


namespace mtg {
namespace graph {

using mtg::common::logger;

inline const DBGSuccinct* get_dbg_succ(const DeBruijnGraph &graph) {
    const DeBruijnGraph *base_graph = &graph;
    while (const auto *wrapper = dynamic_cast<const DBGWrapper<>*>(base_graph)) {
        base_graph = &wrapper->get_graph();
    }

    return dynamic_cast<const DBGSuccinct*>(base_graph);
}

CanonicalDBG::CanonicalDBG(std::shared_ptr<const DeBruijnGraph> graph, size_t cache_size)
      : DBGWrapper<DeBruijnGraph>(std::move(graph)),
        cache_size_(cache_size), is_palindrome_cache_(cache_size_),
        offset_(graph_->max_index()),
        k_odd_(graph_->get_k() % 2) {
    assert(graph_->get_mode() == DeBruijnGraph::PRIMARY
                && "Only primary graphs can be wrapped in CanonicalDBG");

    has_sentinel_ = false;
    alphabet_encoder_.fill(graph_->alphabet().size());

    for (size_t i = 0; i < graph_->alphabet().size(); ++i) {
        alphabet_encoder_[graph_->alphabet()[i]] = i;
        if (graph_->alphabet()[i] == boss::BOSS::kSentinel)
            has_sentinel_ = true;
    }
}

void CanonicalDBG
::map_to_nodes_sequentially(std::string_view sequence,
                            const std::function<void(node_index)> &callback,
                            const std::function<bool()> &terminate) const {
    if (sequence.size() < get_k())
        return;

    std::vector<node_index> path;
    path.reserve(sequence.size() - get_k() + 1);

    // map until the first mismatch
    bool stop = false;
    graph_->map_to_nodes_sequentially(sequence,
        [&](node_index node) {
            if (node) {
                path.push_back(node);
            } else {
                stop = true;
            }
        },
        [&]() { return stop; }
    );

    for (node_index node : path) {
        if (terminate())
            return;

        callback(node);
    }

    // trim the mapped prefix
    sequence = sequence.substr(path.size());
    path.resize(0);
    if (sequence.size() < get_k())
        return;

    std::string rev_seq(sequence);
    ::reverse_complement(rev_seq.begin(), rev_seq.end());
    // map the reverse-complement
    std::vector<node_index> rev_path = graph::map_to_nodes_sequentially(*graph_, rev_seq);

    // map the forward
    const auto *dbg_succ = get_dbg_succ(*graph_);
    if (dbg_succ && get_k() % 2) {
        // if it's a boss table with odd k (without palindromic k-mers),
        // we can skip k-mers that have been found in the rev-compl sequence
        const auto &boss = dbg_succ->get_boss();
        // the initial forward mapping stopped on this k-mer,
        // hence it's missing and we skip it
        path.push_back(npos);
        auto it = rev_path.rbegin() + 1;
        auto is_missing = get_missing_kmer_skipper(dbg_succ->get_bloom_filter(),
                                                   sequence.substr(1));
        boss.map_to_edges(sequence.substr(1),
            [&](boss::BOSS::edge_index edge) {
                path.push_back(dbg_succ->boss_to_kmer_index(edge));
                ++it;
            },
            []() { return false; },
            [&]() {
                if (is_missing() || *it) {
                    path.push_back(npos);
                    ++it;
                    return true;
                } else {
                    return false;
                }
            }
        );

        assert(it == rev_path.rend());

    } else {
        path = graph::map_to_nodes_sequentially(*graph_, sequence);
    }

    assert(path.size() == rev_path.size());

    auto it = rev_path.rbegin();
    for (auto jt = path.begin(); jt != path.end(); ++jt, ++it) {
        if (terminate())
            return;

        if (*jt != npos) {
            callback(*jt);
        } else if (*it != npos) {
            callback(*it + offset_);
        } else {
            callback(npos);
        }
    }
}

void CanonicalDBG::map_to_nodes(std::string_view sequence,
                                const std::function<void(node_index)> &callback,
                                const std::function<bool()> &terminate) const {
    map_to_nodes_sequentially(sequence, [&](node_index i) {
        callback(get_base_node(i));
    }, terminate);
}

void CanonicalDBG::append_next_rc_nodes(node_index node,
                                        SmallVector<node_index> &children) const {
    /**
     *
     * find children of node by searching for parents of its reverse complement
     * e.g., node = ATGGCT. Find TGGCTA and TGGCTT by looking for  TAGCCA and AAGCCA
     *         TGGCTA      TAGCCA
     *        /                  \
     *  ATGGCT         ->         AGCCAT
     *        \                  /
     *         TGGCTT      AAGCCA
     */

    // for each n, check for nAGCCA. If found, define and store the index for
    // TGGCTrc(n) as index(nAGCCA) + offset_

    if (const DBGSuccinct *dbg_succ = get_dbg_succ(*graph_)) {
        const boss::BOSS &boss = dbg_succ->get_boss();

        const auto cache = get_extension<NodeFirstCache>();

        //   AGAGGATCTCGTATGCCGTCTTCTGCTTGAG
        //->  GAGGATCTCGTATGCCGTCTTCTGCTTGAG
        //->  CTCAAGCAGAAGACGGCATACGAGATCCTC
        std::string rev_seq = graph_->get_node_sequence(node).substr(1, boss.get_k());
        if (rev_seq[0] == boss::BOSS::kSentinel)
            return;

        ::reverse_complement(rev_seq.begin(), rev_seq.end());
        auto encoded = boss.encode(rev_seq);
        auto [rc_edge, edge_2, end] = boss.index_range(encoded.begin(), encoded.end());
        if (end != encoded.end())
            return;

        assert(rc_edge == edge_2);

        // rc_edge may be a dummy sink, so this won't work with graph_->call_incoming_kmers
        boss.call_incoming_to_target(boss.bwd(rc_edge), boss.get_node_last_value(rc_edge),
            [&](boss::BOSS::edge_index incoming_boss_edge) {
                node_index next = dbg_succ->boss_to_kmer_index(incoming_boss_edge);
                if (next == npos)
                    return;

                char c = cache
                    ? cache->get_first_char(next)
                    : boss.decode(boss.get_minus_k_value(incoming_boss_edge, boss.get_k() - 1).first);
                if (c == boss::BOSS::kSentinel)
                    return;

                auto s = kmer::KmerExtractorBOSS::complement(boss.encode(c));

                if (children[s] == npos) {
                    children[s] = next + offset_;
                    return;
                }

                if (k_odd_) {
                    logger->error(
                        "Primary graph contains both forward and reverse complement: {} {} -> {} {}\t{} {}",
                        node, graph_->get_node_sequence(node),
                        children[s], graph_->get_node_sequence(children[s]),
                        next, graph_->get_node_sequence(next));
                    exit(1);
                }

                is_palindrome_cache_.Put(next, true);
            }
        );

        return;
    }

    //        rshift    rc
    // ATGGCT -> TGGCT* -> *AGCCA
    std::string rev_seq = get_node_sequence(node).substr(1) + std::string(1, '\0');
    ::reverse_complement(rev_seq.begin(), rev_seq.end());
    assert(rev_seq[0] == '\0');

    const auto &alphabet = graph_->alphabet();
    for (size_t c = 0; c < alphabet.size(); ++c) {
        // Do the checks by directly mapping the sequences of the desired k-mers.
        // For non-DBGSuccinct graphs, this should be fast enough.
        if (alphabet[c] != boss::BOSS::kSentinel && children[c] == npos) {
            rev_seq[0] = complement(alphabet[c]);
            node_index next = graph_->kmer_to_node(rev_seq);
            if (next != npos)
                children[c] = next + offset_;
        }
    }
}

void CanonicalDBG
::call_outgoing_kmers(node_index node, const OutgoingEdgeCallback &callback) const {
    assert(node);
    assert(node <= offset_ * 2);
    if (node > offset_) {
        call_incoming_kmers(node - offset_, [&](node_index next, char c) {
            c = complement(c);
            callback(reverse_complement(next), c);
            assert(traverse(node, c) == reverse_complement(next));
        });
        return;
    }

    const auto &alphabet = graph_->alphabet();

    SmallVector<node_index> children(alphabet.size(), npos);
    size_t max_num_edges_left = children.size() - has_sentinel_;

    graph_->call_outgoing_kmers(node, [&](node_index next, char c) {
        if (c != boss::BOSS::kSentinel) {
            children[alphabet_encoder_[c]] = next;
            --max_num_edges_left;
        }
    });

    if (max_num_edges_left)
        append_next_rc_nodes(node, children);

    for (size_t c = 0; c < children.size(); ++c) {
        if (children[c] != npos) {
            callback(children[c], alphabet[c]);
            assert(traverse(node, alphabet[c]) == children[c]);
        }
    }
}

void CanonicalDBG::append_prev_rc_nodes(node_index node,
                                        SmallVector<node_index> &parents) const {
    /**
     * find parents of node by searching for children of its reverse complement
     * e.g., node = AGCCAT. Find TAGCCA and AAGCCA by looking for TGGCTA and TGGCTT.
     *  TAGCCA                    TGGCTA
     *        \                  /
     *         AGCCAT  ->  ATGGCT
     *        /                  \
     *  AAGCCA                    TGGCTT
     */

    // for each n, check for TGGCTn. If found, define and store the index for
    // rc(n)AGCCA as index(TGGCTn) + offset_

    if (const DBGSuccinct *dbg_succ = get_dbg_succ(*graph_)) {
        const boss::BOSS &boss = dbg_succ->get_boss();

        //   AGAGGATCTCGTATGCCGTCTTCTGCTTGAG
        //-> AGAGGATCTCGTATGCCGTCTTCTGCTTGA
        //-> TCAAGCAGAAGACGGCATACGAGATCCTCT
        std::string rev_seq = graph_->get_node_sequence(node).substr(0, boss.get_k());
        if (rev_seq[0] == boss::BOSS::kSentinel)
            return;

        ::reverse_complement(rev_seq.begin(), rev_seq.end());
        auto encoded = boss.encode(rev_seq);
        auto [rc_edge, edge_2, end] = boss.index_range(encoded.begin(), encoded.end());

        if (end != encoded.end())
            return;

        assert(rc_edge == edge_2);

        boss.call_outgoing(rc_edge, [&](boss::BOSS::edge_index adjacent_edge) {
            assert(dbg_succ);
            node_index prev = dbg_succ->boss_to_kmer_index(adjacent_edge);
            if (prev == npos)
                return;

            boss::BOSS::TAlphabet c = boss.get_W(adjacent_edge) % boss.alph_size;

            if (c == boss::BOSS::kSentinelCode)
                return;

            c = kmer::KmerExtractorBOSS::complement(c);

            if (parents[c] == npos) {
                parents[c] = prev + offset_;

                return;
            }

            if (k_odd_) {
                logger->error(
                    "Primary graph contains both forward and reverse complement: {} {} <- {} {}\t{} {}",
                    node, graph_->get_node_sequence(node),
                    parents[c], graph_->get_node_sequence(parents[c]),
                    prev, graph_->get_node_sequence(prev)
                );
                exit(1);
            }

            is_palindrome_cache_.Put(prev, true);
        });

        return;
    }

    //        lshift    rc
    // AGCCAT -> *AGCCA -> TGGCT*
    std::string rev_seq = get_node_sequence(node).substr(0, get_k() - 1);
    ::reverse_complement(rev_seq.begin(), rev_seq.end());
    rev_seq.push_back('\0');

    const auto &alphabet = graph_->alphabet();
    for (size_t c = 0; c < alphabet.size(); ++c) {
        // Do the checks by directly mapping the sequences of the desired k-mers.
        // For non-DBGSuccinct graphs, this should be fast enough.
        if (alphabet[c] != boss::BOSS::kSentinel && parents[c] == npos) {
            rev_seq.back() = complement(alphabet[c]);
            node_index prev = graph_->kmer_to_node(rev_seq);
            if (prev != npos)
                parents[c] = prev + offset_;
        }
    }
}

void CanonicalDBG
::call_incoming_kmers(node_index node, const IncomingEdgeCallback &callback) const {
    assert(node);
    assert(node <= offset_ * 2);
    if (node > offset_) {
        call_outgoing_kmers(node - offset_, [&](node_index prev, char c) {
            c = complement(c);
            callback(reverse_complement(prev), c);
            assert(traverse_back(node, c) == reverse_complement(prev));
        });
        return;
    }

    const auto &alphabet = graph_->alphabet();

    SmallVector<node_index> parents(alphabet.size(), npos);
    size_t max_num_edges_left = parents.size() - has_sentinel_;

    auto incoming_kmer_callback = [&](node_index prev, char c) {
        if (c != boss::BOSS::kSentinel) {
            parents[alphabet_encoder_[c]] = prev;
            --max_num_edges_left;
        }
    };

    if (const auto cache = get_extension<NodeFirstCache>()) {
        cache->call_incoming_kmers(node, incoming_kmer_callback);
    } else {
        graph_->call_incoming_kmers(node, incoming_kmer_callback);
    }

    if (max_num_edges_left)
        append_prev_rc_nodes(node, parents);

    for (size_t c = 0; c < parents.size(); ++c) {
        if (parents[c] != npos) {
            callback(parents[c], alphabet[c]);
            assert(traverse_back(node, alphabet[c]) == parents[c]);
        }
    }
}

void CanonicalDBG
::adjacent_outgoing_nodes(node_index node,
                          const std::function<void(node_index)> &callback) const {
    // TODO: better implementation
    call_outgoing_kmers(node, [&](node_index i, char) { callback(i); });
}

void CanonicalDBG
::adjacent_incoming_nodes(node_index node,
                          const std::function<void(node_index)> &callback) const {
    // TODO: better implementation
    call_incoming_kmers(node, [&](node_index i, char) { callback(i); });
}

size_t CanonicalDBG::outdegree(node_index node) const {
    // TODO: better implementation
    size_t outdegree = 0;
    adjacent_outgoing_nodes(node, [&](node_index) { ++outdegree; });
    return outdegree;
}

size_t CanonicalDBG::indegree(node_index node) const {
    // TODO: better implementation
    size_t indegree = 0;
    adjacent_incoming_nodes(node, [&](node_index) { ++indegree; });
    return indegree;
}

void CanonicalDBG::call_sequences(const CallPath &callback,
                                  size_t num_threads,
                                  bool kmers_in_single_form) const {
    if (kmers_in_single_form) {
        graph_->call_sequences(callback, num_threads, false);
    } else {
        // TODO: port over implementation from DBGSuccinct to DeBruijnGraph
        DeBruijnGraph::call_sequences(callback, num_threads, false);
    }
}

void CanonicalDBG::call_unitigs(const CallPath &callback,
                                size_t num_threads,
                                size_t min_tip_size,
                                bool kmers_in_single_form) const {
    // TODO: port over implementation from DBGSuccinct to DeBruijnGraph
    DeBruijnGraph::call_unitigs(callback, num_threads, min_tip_size, kmers_in_single_form);
}

std::string CanonicalDBG::get_node_sequence(node_index index) const {
    assert(index <= offset_ * 2);
    node_index node = get_base_node(index);
    std::string seq = graph_->get_node_sequence(node);

    if (node != index)
        ::reverse_complement(seq.begin(), seq.end());

    return seq;
}

DeBruijnGraph::node_index CanonicalDBG::traverse(node_index node, char next_char) const {
    assert(node <= offset_ * 2);
    if (node > offset_) {
        node = traverse_back(node - offset_, complement(next_char));
        return node != npos ? reverse_complement(node) : npos;
    } else {
        node_index next = graph_->traverse(node, next_char);
        if (next != npos)
            return next;

        std::string rev_seq = get_node_sequence(node).substr(1) + next_char;
        ::reverse_complement(rev_seq.begin(), rev_seq.end());
        next = graph_->kmer_to_node(rev_seq);
        return next != npos ? reverse_complement(next) : next;
    }
}

DeBruijnGraph::node_index CanonicalDBG::traverse_back(node_index node,
                                                      char prev_char) const {
    assert(node <= offset_ * 2);
    if (node > offset_) {
        node = traverse(node - offset_, complement(prev_char));
        return node != npos ? reverse_complement(node) : npos;
    } else {
        node_index prev = graph_->traverse_back(node, prev_char);
        if (prev != npos)
            return prev;

        std::string rev_seq = std::string(1, prev_char)
            + get_node_sequence(node).substr(0, get_k() - 1);
        ::reverse_complement(rev_seq.begin(), rev_seq.end());
        prev = graph_->kmer_to_node(rev_seq);
        return prev != npos ? reverse_complement(prev) : prev;
    }
}

void CanonicalDBG::call_nodes(const std::function<void(node_index)> &callback,
                              const std::function<bool()> &stop_early) const {
    graph_->call_nodes(
        [&](node_index i) {
            callback(i);
            if (!stop_early()) {
                node_index j = reverse_complement(i);
                if (j != i)
                    callback(j);
            }
        },
        stop_early
    );
}

void CanonicalDBG
::call_kmers(const std::function<void(node_index, const std::string&)> &callback) const {
    graph_->call_kmers([&](node_index i, const std::string &seq) {
        callback(i, seq);
        node_index j = reverse_complement(i);
        if (j != i) {
            std::string rseq(seq);
            ::reverse_complement(rseq.begin(), rseq.end());
            callback(j, rseq);
        }
    });
}

bool CanonicalDBG::operator==(const DeBruijnGraph &other) const {
    if (dynamic_cast<const CanonicalDBG*>(&other)) {
        return *this == dynamic_cast<const CanonicalDBG&>(other);
    }

    return DeBruijnGraph::operator==(other);
}

DeBruijnGraph::node_index CanonicalDBG::reverse_complement(node_index node) const {
    assert(node);
    assert(node <= offset_ * 2);

    if (node > offset_) {
        // we know that this node is definitely not present in the base graph

        if (!k_odd_)
            is_palindrome_cache_.Put(node - offset_, false);

        return node - offset_;

    } else if (k_odd_) {
        // if k is odd, then we know that the reverse complement doesn't exist,
        // so we apply the offset
        return node + offset_;

    }

    if (auto fetch = is_palindrome_cache_.TryGet(node))
        return *fetch ? node : node + offset_;

    std::string seq = graph_->get_node_sequence(node);
    std::string rev_seq = seq;
    ::reverse_complement(rev_seq.begin(), rev_seq.end());
    bool palindrome = (rev_seq == seq);

    assert(palindrome || graph_->kmer_to_node(rev_seq) == npos);

    is_palindrome_cache_.Put(node, palindrome);
    return palindrome ? node : node + offset_;
}

void CanonicalDBG::reverse_complement(std::string &seq,
                                      std::vector<node_index> &path) const {
    ::reverse_complement(seq.begin(), seq.end());

    std::vector<node_index> rev_path(path.size());
    std::transform(path.begin(), path.end(), rev_path.rbegin(), [&](node_index i) {
        return i ? reverse_complement(i) : i;
    });
    std::swap(path, rev_path);
}

} // namespace graph
} // namespace mtg
