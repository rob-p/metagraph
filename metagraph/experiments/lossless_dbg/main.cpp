#include <utility>
#include <iostream>
#include <map>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <ProgressBar.hpp>
#include <tclap/CmdLine.h>
#include <gtest/gtest.h>

using TCLAP::ValueArg;
using TCLAP::MultiArg;
using TCLAP::UnlabeledValueArg;
using TCLAP::UnlabeledMultiArg;
using TCLAP::ValuesConstraint;

using json = nlohmann::json;
#define _DNA_GRAPH 1

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#pragma clang diagnostic ignored "-Wcomma"

#include "dbg_succinct.hpp"
#include "sequence_graph.hpp"
#include "sequence_io.hpp"
#include "dbg_succinct_construct.hpp"
#include "dbg_hash.hpp"



#pragma clang diagnostic pop

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

#include "compressed_reads.hpp"

using namespace std;
using namespace std::string_literals;
namespace fs = std::filesystem;

// debugging functions
struct d_t {
    template<typename T> d_t & operator,(const T & x) {
        std::cerr << ' ' <<  x;
        return *this;
    }
};

#define D(args ...) { d_t, "|", __LINE__, "|", #args, ":", args, "\n"; }

using node_index = SequenceGraph::node_index;

string local_file(string filename) {
    return fs::path(__FILE__).parent_path() / filename;
}

// TODO: remove these constants
string HUMAN_REFERENCE_FILENAME = local_file("genomic_data/GCF_000001405.38_GRCh38.p12_genomic.fna");
string HUMAN_CHROMOSOME_10_SAMPLE = local_file("genomic_data/human_chromosome_10_sample.fasta");
string HUMAN_CHROMOSOME_10_FILENAME = local_file("genomic_data/human_chromosome_10.fasta");
string HUMAN_CHROMOSOME_10_STRIPPED_N_FILENAME = local_file("genomic_data/human_chromosome_10_n_trimmed.fasta");
string JSON_OUTPUT_FILE = local_file("statistics.json");
const int CHROMOSOME_NUMBER = 10;
const int READ_LENGTH = 100;
const double READ_COVERAGE = 0.00001;
const int test_seed = 3424;
const int DEFAULT_K_KMER = 21;
// todo change to proper thing
#define x first
#define y second
#define all(x) begin(x),end(x)

// openmp reductions

void reduce_maps(\
                 std::map<int, int>& output, \
                 std::map<int, int>& input)
{
    for (auto& X : input) {
        output[X.first] += X.second;
    }
}

#pragma omp declare reduction(map_reduction : \
std::map<int, int> : \
reduce_maps(omp_out, omp_in)) \
initializer(omp_priv(omp_orig))

class SamplerConvenient {
public:
    virtual string sample(int length) = 0;
    virtual int reference_size() = 0;
    virtual vector<string> sample_coverage(int length, double coverage) {
        int count = ceil(reference_size()*coverage/length);
        return sample(length, count);
    }
    virtual vector<string> sample(int length, int count) {
        auto res = vector<string>();
        for(int i=0;i<count;i++) {
            res.push_back(sample(length));
        }
        return res;
    }
};

class Sampler : public SamplerConvenient {
public:
    Sampler(string reference, unsigned int seed) : reference(std::move(reference)) {
        generator = std::mt19937(seed); //Standard mersenne_twister_engine seeded with rd()
    };
    string sample(int length) override {
        std::uniform_int_distribution<> dis(0, reference.length()-1-length);
        return reference.substr(dis(generator),length);
    }
    int reference_size() override {
        return reference.size();
    }
private:
    string reference;
    std::mt19937 generator;
};

class DeterministicSampler : public SamplerConvenient {
public:
    DeterministicSampler(vector<string> samples, int reference_size) : _reference_size(reference_size), samples(std::move(samples)) {};
    string sample(int length) override {
        string sample = samples[current_sample];
        assert(length==sample.length());
        current_sample = (current_sample + 1) % samples.size();
        return sample;
    }
    int reference_size() override {
        return _reference_size;
    }
    vector<string> samples;
    int _reference_size;
    int current_sample = 0;
};

void transform_to_fasta(const string &filename,vector<string> reads) {
    ofstream myfile;
    myfile.open (filename);
    for(auto& read : reads) {
        myfile << ">" << endl;
        myfile << read << endl;
    }
    myfile.close();
}

vector<string> read_reads_from_fasta(const string &filename) {
    vector<string> result;
    read_fasta_file_critical(
                             filename,
                             [&](kseq_t* read) {
                                 result.push_back(read->seq.s);
                             });
    return result;
}

string get_human_chromosome(int chromosome_number,bool five_letter_alphabet=true) {
    int current_chromosome=1;
    string result;
    read_fasta_file_critical(
                             HUMAN_REFERENCE_FILENAME,
                             [&](kseq_t* chromosome) {
                                 if (chromosome->comment.s == ("Homo sapiens chromosome "s + to_string(chromosome_number) + ", GRCh38.p12 Primary Assembly"s)) {
                                     result = chromosome->seq.s;
                                 }
                                 current_chromosome++;
                             });
    if (five_letter_alphabet) {
        transform(all(result),result.begin(),::toupper);
    }
    return result;
}

TEST(SamplerTest,SampleNoRandom) {
    auto sampler = Sampler("AAAAAAAAA",test_seed);
    ASSERT_EQ(sampler.sample(2),"AA");
}

TEST(SamplerTest,SampleNormal) {
    auto sampler = Sampler("ADFAGADFDS",test_seed);
    ASSERT_EQ(sampler.sample(4),"ADFD");
}
TEST(SamplerTest,SampleCoverage) {
    auto sequence = "ADFAGADFDS"s;
    auto sampler = Sampler(sequence,test_seed);
    auto reads = sampler.sample_coverage(sequence.length()/2, 1);
    ASSERT_EQ(reads.size(), 2);
}

TEST(CompressingReads,GetChromosomeWorks) {
    auto chromosome = get_human_chromosome(CHROMOSOME_NUMBER);
    EXPECT_EQ(chromosome.length(), 133'797'422);
    EXPECT_EQ(chromosome.substr(0,10),"NNNNNNNNNN");
}

TEST(CompressedReads,IdentityTest1) {
    set<string> reads = {"ATGCGATCGATATGCGAGA",
                         "ATGCGATCGAGACTACGAG",
                         "GTACGATAGACATGACGAG",
                         "ACTGACGAGACACAGATGC"};
    auto compressed_reads = CompressedReads(vector<string>(all(reads)));
    auto decompressed_reads = compressed_reads.get_reads();
    set<string> decompressed_read_set = set<string>(all(decompressed_reads));
    ASSERT_EQ(reads, decompressed_read_set);
}

void to_be_determined() {
    auto chromosome = get_human_chromosome(CHROMOSOME_NUMBER);
    auto sampler = Sampler(chromosome,test_seed);
    auto reads = sampler.sample_coverage(READ_LENGTH, READ_COVERAGE);
    transform_to_fasta(HUMAN_CHROMOSOME_10_SAMPLE,reads);
    auto compressed_reads = CompressedReads(reads);
}
void code_to_violate_assertion() {
    auto reads = read_reads_from_fasta(HUMAN_CHROMOSOME_10_SAMPLE);
    auto compressed_reads = CompressedReads(reads);
}
void save_human_chromosome() {
    auto chromosome = get_human_chromosome(CHROMOSOME_NUMBER);
    transform_to_fasta(HUMAN_CHROMOSOME_10_FILENAME,{chromosome});
}

void get_statistics() {
    auto chromosome_cleaned
        = read_reads_from_fasta(HUMAN_CHROMOSOME_10_STRIPPED_N_FILENAME)[0];

    DBGHash graph(DEFAULT_K_KMER);

    graph.add_sequence(chromosome_cleaned);

    int kmers_count = graph.num_nodes();

    auto pb = ProgressBar(kmers_count, 70, '=', ' ', 10000);

    map<int,int> kmer_outgoing_edges_statistics;

    // openmp doesn't work with maps
    //#pragma omp parallel for reduction(map_reduction:kmer_outgoing_edges_statistics)
    //for(auto it = graph.indices_.begin(); it != graph.indices_.end(); it++)
    vector<node_index> outgoing_edges;

    for (size_t i = 1; i <= graph.num_nodes(); ++i) {
        // const auto &kmer = graph.get_node_sequence(i);
        graph.adjacent_outgoing_nodes(i, &outgoing_edges);

        kmer_outgoing_edges_statistics[outgoing_edges.size()]++;

        outgoing_edges.clear();

        ++pb;
        pb.display();
    }

    json statistics(kmer_outgoing_edges_statistics);

    ofstream myfile;

    myfile.open(JSON_OUTPUT_FILE);

    myfile << statistics.dump(4) << endl;
    cout << statistics.dump(4) << endl;
}


int main(int argc, char *argv[]) {
    TCLAP::CmdLine cmd("Compress reads",' ', "0.1");
    TCLAP::ValueArg<std::string> nameArg("r",
                                         "reference",
                                         "path to human reference",
                                         false,
                                         HUMAN_REFERENCE_FILENAME,
                                         "string");
    cmd.add(nameArg);
    cmd.parse( argc, argv );
    HUMAN_REFERENCE_FILENAME = nameArg.getValue();
    get_statistics();
    //save_human_chromosome();
    //playground_dbg();
    //to_be_determined();
    //    code_to_violate_assertion();
    //    ::testing::InitGoogleTest(&argc, argv);
    //    return RUN_ALL_TESTS();
}
