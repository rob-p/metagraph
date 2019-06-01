//
// Created by Jan Studený on 2019-03-11.
//
#include <utility>
#include <iostream>
#include <map>
#include <filesystem>
#include <tclap/CmdLine.h>
#include <random>
#include <nlohmann/json.hpp>

using TCLAP::ValueArg;
using TCLAP::MultiArg;
using TCLAP::UnlabeledValueArg;
using TCLAP::UnlabeledMultiArg;
using TCLAP::ValuesConstraint;

#include "graph_patch.hpp"
#include "utilities.hpp"


#define STATS_INCOMING_HISTOGRAM 1
#define STATS_OUTGOING_HISTOGRAM 2

using namespace std;
using namespace nlohmann;
using namespace std::string_literals;

json get_statistics(DBGSuccinct& graph,int verbosity=~0) {
    Timer timer;
    cerr << "Starting computation of graph statistics" << endl;

    int joins = 0;
    int splits = 0;
    int incoming_histogram[6] = {0,0,0,0,0,0};
    int outgoing_histogram[6] = {0,0,0,0,0,0};
    #pragma omp parallel for reduction(+:joins,splits,incoming_histogram[:6],outgoing_histogram[:6])
    for (int node = 1; node <= graph.num_nodes();node++) {
        int indegree = graph.indegree(node);
        int outdegree = graph.outdegree(node);
        if (verbosity & STATS_INCOMING_HISTOGRAM) {
            incoming_histogram[indegree]++;
        }
        if (verbosity & STATS_OUTGOING_HISTOGRAM) {
            outgoing_histogram[outdegree]++;
        }
        if (indegree > 1) {
            joins++;
        }
        if (outdegree > 1) {
            splits++;
        }
    }
    json result{{"joins",joins},
                {"splits",splits},
                {"incoming_histogram", incoming_histogram},
                {"outgoing_histogram", outgoing_histogram},
                {"num_of_nodes", graph.num_nodes()}
                };
    cerr << "Computation of statistics finished in " << timer.elapsed() << " sec." << endl;
    return result;
}

int main_statistics(int argc, char *argv[]) {
    TCLAP::CmdLine cmd("Compress reads",' ', "0.1");
    TCLAP::ValueArg<std::string> graphArg("g",
                                          "graph",
                                          "Graph to use as a reference in compression",
                                          true,
                                          "",
                                          "string",cmd);
    TCLAP::ValueArg<std::string> statisticsArg("s",
                                               "statistics",
                                               "Filename of json file that will output statistics about compressed file.",
                                               true,
                                               "statistics.json",
                                               "string",cmd);
    TCLAP::ValueArg<int> verbosityArg("v",
                                               "verbosity",
                                               "Level of detail of the statistics",
                                               false,
                                               0,
                                               "int",cmd);
    cmd.parse(argc, argv);
    auto graph = DBGSuccinct(21);
    graph.load(graphArg.getValue());
    auto statistics = get_statistics(graph,verbosityArg.getValue());
    cout << statistics << endl;
    save_string(statistics.dump(4),statisticsArg.getValue());
    return 0;
}
