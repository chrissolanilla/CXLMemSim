#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "policy.h"

#include "policy_test_common.h"
#include "paper_fraction_model.h"

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

    auto* exp = new CXLMemExpander(
        20000, 20000, 200, 250, 0, 256
    );

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

void write_csv(const std::string& filename, const std::vector<ExperimentResult>& results) {
    std::ofstream out(filename);
    if (!out) {
        std::cerr << "failed to open " << filename << "\n";
        return;
    }

    out << "policy,chosen_target_fraction,final_fraction,local_pages,remote_pages,"
           "moved_pages,promoted_pages,demoted_pages,rounds_with_migration,"
           "cost1_in_cxl,cost2_policy,g_of_x\n";

    for (const auto& r : results) {
        out << r.policy_name << ","
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

} // namespace

int main() {
    ScenarioConfig scenario{};

    // Start small so the target fraction is actually reachable.
    scenario.num_pages = 64;
    scenario.hot_pages = 8;
    scenario.rounds = 8;
    scenario.accesses_per_round = 200;
    scenario.batch_size = 8;
    scenario.tolerance = 0.05;

    // Paper-inspired parameters.
    // These are not measured from CXLMemSim yet; they are placeholders.
    // TODO: replace with profiled values from your simulator or experiments.
    scenario.r = 1.0;
    scenario.k = 2.3534;
    scenario.m = 1.0977;
    scenario.alpha = 0.60;

    std::vector<ExperimentResult> results;
    results.push_back(run_policy(scenario, PolicyKind::NoMigration));
    results.push_back(run_policy(scenario, PolicyKind::FullMigration));
    results.push_back(run_policy(scenario, PolicyKind::NaiveHotness));
    results.push_back(run_policy(scenario, PolicyKind::PaperPlannedFractionGuided));

    std::cout << "=== Paper-Inspired Policy Comparison ===\n";
    for (const auto& r : results) {
        std::cout << r.policy_name
                  << " | chosen_target=" << r.chosen_target_fraction
                  << " | final_fraction=" << r.final_fraction
                  << " | moved=" << r.moved_pages
                  << " | realized cost(cost2)=" << r.realized_cost
                  << " | speedup vs baseline(g(x)) =" << r.speedup_vs_baseline
                  << "\n";
    }

    write_csv("policy_comparison.csv", results);
    std::cout << "wrote policy_comparison.csv\n";
    return 0;
}
