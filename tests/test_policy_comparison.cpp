#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "policy.h"

#include "policy_test_common.h"
#include "paper_fraction_model.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

Helper helper{};
CXLController* controller = nullptr;
Monitors* monitors = nullptr;

namespace {

double local_fraction(CXLController& ctrl, CXLMemExpander& exp) {
    const std::size_t local_pages = ctrl.occupation.size();
    const std::size_t remote_pages = exp.occupation.size();
    const std::size_t total_pages = local_pages + remote_pages;
    if (total_pages == 0) return 0.0;
    return static_cast<double>(local_pages) / static_cast<double>(total_pages);
}

class NoMigrationPolicy : public MigrationPolicy {
public:
    int compute_once(CXLController*) override { return 0; }

    std::vector<std::tuple<uint64_t, uint64_t>>
    get_migration_list(CXLController*) override {
        return {};
    }
};

class FullMigrationPolicy : public MigrationPolicy {
public:
    std::vector<std::tuple<uint64_t, uint64_t>>
    get_migration_list(CXLController* controller) override {
        std::vector<std::tuple<uint64_t, uint64_t>> out;
        constexpr uint64_t page_size = 4096;

        for (auto* exp : controller->expanders) {
            for (const auto& info : exp->occupation) {
                out.emplace_back(info.address, page_size);
            }
        }
        return out;
    }
};

class NaiveHotnessMigrationPolicy : public MigrationPolicy {
public:
    explicit NaiveHotnessMigrationPolicy(int batch_size)
        : batch_size_(batch_size) {}

    void record_access(uint64_t addr) {
        access_count_[addr]++;
    }

    std::vector<std::tuple<uint64_t, uint64_t>>
    get_migration_list(CXLController* controller) override {
        std::vector<std::pair<uint64_t, uint64_t>> candidates;
        constexpr uint64_t page_size = 4096;

        for (auto* exp : controller->expanders) {
            for (const auto& info : exp->occupation) {
                candidates.push_back({access_count_[info.address], info.address});
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });

        std::vector<std::tuple<uint64_t, uint64_t>> out;
        const int take = std::min(batch_size_, static_cast<int>(candidates.size()));
        for (int i = 0; i < take; i++) {
            out.emplace_back(candidates[i].second, page_size);
        }
        return out;
    }

private:
    int batch_size_;
    std::unordered_map<uint64_t, uint64_t> access_count_;
};

enum class PolicyKind {
    NoMigration,
    FullMigration,
    NaiveHotness,
    PaperPlannedFractionGuided
};

const char* policy_name(PolicyKind kind) {
    switch (kind) {
        case PolicyKind::NoMigration: return "no_migration";
        case PolicyKind::FullMigration: return "full_migration";
        case PolicyKind::NaiveHotness: return "naive_hotness";
        case PolicyKind::PaperPlannedFractionGuided: return "paper_fraction_guided";
    }
    return "unknown";
}

ExperimentResult run_policy(const ScenarioConfig& scenario, PolicyKind kind) {
    AllocationPolicy* allocation = new AllocationPolicy();
    PagingPolicy* paging = new PagingPolicy();
    CachingPolicy* caching = new CachingPolicy();

    MigrationPolicy* migration = nullptr;
    NaiveHotnessMigrationPolicy* naive = nullptr;
    FractionGuidedMigrationPolicy* fraction_guided = nullptr;

    double chosen_target_fraction = 0.0;

    if (kind == PolicyKind::NoMigration) {
        migration = new NoMigrationPolicy();
    } else if (kind == PolicyKind::FullMigration) {
        migration = new FullMigrationPolicy();
        chosen_target_fraction = 1.0;
    } else if (kind == PolicyKind::NaiveHotness) {
        naive = new NaiveHotnessMigrationPolicy(scenario.batch_size);
        migration = naive;
    } else {
        const PaperModelResult paper =
            evaluate_paper_model(scenario.r, scenario.k, scenario.m, scenario.alpha);

        chosen_target_fraction = paper.x_star;

        // if the paper says movement is not worth it, just behave like no-migration
        if (!paper.movement_worth_it) {
            migration = new NoMigrationPolicy();
        } else {
            fraction_guided = new FractionGuidedMigrationPolicy(
                chosen_target_fraction,
                scenario.tolerance,
                scenario.batch_size
            );
            migration = fraction_guided;
        }
    }

    std::array<Policy*, 4> policies = {
        allocation,
        migration,
        paging,
        caching
    };

    CXLController ctrl(policies, 256, page_type::PAGE, 10, 100.0);
    controller = &ctrl;

    auto* exp = new CXLMemExpander(20000, 20000, 200, 250, 0, 256);

    ctrl.insert_end_point(exp);
    ctrl.expanders.push_back(exp);
    ctrl.device_map[0] = exp;
    ctrl.num_end_points = 1;

    for (int i = 0; i < scenario.num_pages; i++) {
        occupation_info info{};
        info.timestamp = i;
        info.address = scenario.base_addr + static_cast<uint64_t>(i) * 4096;
        info.access_count = 0;
        exp->occupation.push_back(info);
    }

    std::size_t moved_pages = 0;
    std::size_t promoted_pages = 0;
    std::size_t demoted_pages = 0;
    int rounds_with_migration = 0;

    for (int round = 1; round <= scenario.rounds; round++) {
        for (int rep = 0; rep < scenario.accesses_per_round; rep++) {
            for (int i = 0; i < scenario.hot_pages; i++) {
                const uint64_t hot_addr =
                    scenario.base_addr + static_cast<uint64_t>(i) * 4096;

                if (naive) naive->record_access(hot_addr);
                if (fraction_guided) fraction_guided->record_access(hot_addr);
            }
        }

        const std::size_t before_local = ctrl.occupation.size();
        const std::size_t before_remote = exp->occupation.size();

        if (migration->compute_once(&ctrl) > 0) {
            ctrl.perform_migration();
        }

        const std::size_t after_local = ctrl.occupation.size();
        const std::size_t after_remote = exp->occupation.size();

        if (before_local != after_local || before_remote != after_remote) {
            rounds_with_migration++;

            if (after_local > before_local) {
                const std::size_t delta = after_local - before_local;
                moved_pages += delta;
                promoted_pages += delta;
            } else {
                const std::size_t delta = before_local - after_local;
                moved_pages += delta;
                demoted_pages += delta;
            }
        }
    }

    const double realized_fraction = local_fraction(ctrl, *exp);

    // Paper-style scoring:
    // cost1 = 1/r
    // cost2(x) = x/(k*r) + 1/((m*x+1)r)
    // Here we evaluate using the realized fraction, since that is what the actuator achieved.
    const double cost1 = paper_cost1(scenario.r);
    const double cost2 = paper_cost2(realized_fraction, scenario.r, scenario.k, scenario.m);
    const double g = paper_g_of_x(realized_fraction, scenario.r, scenario.k, scenario.m);

    ExperimentResult result{};
    result.policy_name = policy_name(kind);
    result.chosen_target_fraction = chosen_target_fraction;
    result.final_fraction = realized_fraction;
    result.local_pages = ctrl.occupation.size();
    result.remote_pages = exp->occupation.size();
    result.moved_pages = moved_pages;
    result.promoted_pages = promoted_pages;
    result.demoted_pages = demoted_pages;
    result.rounds_with_migration = rounds_with_migration;
    result.baseline_cost = cost1;
    result.realized_cost = cost2;
    result.speedup_vs_baseline = g;

    delete allocation;
    delete migration;
    delete paging;
    delete caching;
    delete exp;
    controller = nullptr;

    return result;
}

void write_sweep_csv(const std::string& filename,
                     const std::vector<SweepRow>& rows) {
    std::ofstream out(filename);
    if (!out) {
        std::cerr << "failed to open " << filename << "\n";
        return;
    }

    out << "num_pages,hot_pages,rounds,accesses_per_round,batch_size,tolerance,"
           "r,k,m,alpha,policy,chosen_target_fraction,final_fraction,local_pages,"
           "remote_pages,moved_pages,promoted_pages,demoted_pages,rounds_with_migration,"
           "cost1_in_cxl,cost2_policy,g_of_x\n";

    for (const auto& row : rows) {
        const auto& s = row.scenario;
        const auto& r = row.result;

        out << s.num_pages << ","
            << s.hot_pages << ","
            << s.rounds << ","
            << s.accesses_per_round << ","
            << s.batch_size << ","
            << s.tolerance << ","
            << s.r << ","
            << s.k << ","
            << s.m << ","
            << s.alpha << ","
            << r.policy_name << ","
            << r.chosen_target_fraction << ","
            << r.final_fraction << ","
            << r.local_pages << ","
            << r.remote_pages << ","
            << r.moved_pages << ","
            << r.promoted_pages << ","
            << r.demoted_pages << ","
            << r.rounds_with_migration << ","
            << r.baseline_cost << ","
            << r.realized_cost << ","
            << r.speedup_vs_baseline << "\n";
    }
}

bool has_flag(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; i++) {
        if (argv[i] == flag) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    ScenarioConfig base{};

    // workload defaults
    base.num_pages = 64;
    base.hot_pages = 8;
    base.rounds = 8;
    base.accesses_per_round = 200;
    base.batch_size = 8;
    base.tolerance = 0.05;

    // default paper-model parameters
    base.r = 1.0;
    base.k = 2.3534;
    base.m = 1.0977;
    base.alpha = 0.60;

    const bool sweep_paper_params = has_flag(argc, argv, "--full_sweep");
    const bool verbose = has_flag(argc, argv, "--verbose");

    // existing scenario sweeps if you want them
    const std::vector<int> num_pages_values = {64, 128, 256, 512};
    const std::vector<double> hot_fraction_values = {0.05, 0.10, 0.125, 0.20, 0.25, 0.40};
    const std::vector<int> batch_size_values = {4, 8, 16, 32};

    // paper parameter lists
    std::vector<double> k_values;
    std::vector<double> m_values;
    std::vector<double> alpha_values;

    if (sweep_paper_params) {
        k_values = {1.25, 1.75, 2.0, 2.35, 3.0};
        m_values = {0.25, 0.75, 1.0977, 1.5, 2.0, 2.25, 2.5, 2.75, 3.0};
        alpha_values = {0.20, 0.40, 0.60, 0.80, 1.0};
    } else {
        k_values = {base.k};
        m_values = {base.m};
        alpha_values = {base.alpha};
    }

    const std::vector<PolicyKind> policies = {
        PolicyKind::NoMigration,
        PolicyKind::FullMigration,
        PolicyKind::NaiveHotness,
        PolicyKind::PaperPlannedFractionGuided
    };

    std::vector<SweepRow> rows;

    for (int num_pages : num_pages_values) {
        for (double hot_fraction : hot_fraction_values) {
            for (int batch_size : batch_size_values) {
                for (double k : k_values) {
                    for (double m : m_values) {
                        for (double alpha : alpha_values) {
                            ScenarioConfig scenario = base;
                            scenario.num_pages = num_pages;
                            scenario.hot_pages =
                                std::max(1, static_cast<int>(num_pages * hot_fraction));
                            scenario.batch_size = std::min(batch_size, scenario.hot_pages);

                            scenario.k = k;
                            scenario.m = m;
                            scenario.alpha = alpha;

                            for (PolicyKind policy : policies) {
                                ExperimentResult result = run_policy(scenario, policy);
                                rows.push_back({scenario, result});

                                if (verbose)
                                {
                                    std::cout
                                        << "pages=" << scenario.num_pages
                                        << " hot=" << scenario.hot_pages
                                        << " batch=" << scenario.batch_size
                                        << " k=" << scenario.k
                                        << " m=" << scenario.m
                                        << " alpha=" << scenario.alpha
                                        << " policy=" << result.policy_name
                                        << " chosen_target=" << result.chosen_target_fraction
                                        << " final_fraction=" << result.final_fraction
                                        << " g=" << result.speedup_vs_baseline
                                        << "\n";
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    const std::string out_name =
        sweep_paper_params ? "policy_sweep_full.csv"
                           : "policy_sweep_default.csv";

    write_sweep_csv(out_name, rows);

    std::cout << "wrote " << out_name << " with " << rows.size() << " rows\n";
    return 0;
}