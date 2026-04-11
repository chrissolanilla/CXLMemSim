#include <cstddef>
#include <cstdint>
#include <string>

struct ScenarioConfig {
	//i think if we up the num pages things may not scale so well, would also have to scale hot pages prob
    int num_pages = 64;
    int hot_pages = 8;
    int rounds = 8;
    int accesses_per_round = 200;
    int batch_size = 8;
    double tolerance = 0.05;

    //paper-model parameters
    //r: in-CXL execution throughput baseline
    //k: movement throughput coefficient so p = k * r
    //m: execution throughput slope so q(x) = (m*x + 1) * r
    //alpha: max allowed DRAM fraction
    double r = 1.0;
    double k = 2.35;
    double m = 1.09;
    double alpha = 0.60;

    uint64_t base_addr = 0x700000000ULL;
};

struct ExperimentResult {
    std::string policy_name;

    double chosen_target_fraction = 0.0;
    double final_fraction = 0.0;

    std::size_t local_pages = 0;
    std::size_t remote_pages = 0;

    std::size_t moved_pages = 0;
    std::size_t promoted_pages = 0;
    std::size_t demoted_pages = 0;

    int rounds_with_migration = 0;
    //paper-style metrics
    double baseline_cost = 0.0;
    double realized_cost = 0.0;
	//g(x) value(but the other polices it wouldnt make sense with this name)
    double speedup_vs_baseline = 0.0;
};
