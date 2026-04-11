/*
 * Experiment harness for fraction-guided migration.
 *
 * This test file exercises three related ideas:
 *   1. target-fraction convergence,
 *   2. model-driven target selection,
 *   3. weighted-hotness page prioritization.
 *
 * The harness uses synthetic remote/local page populations and
 * synthetic cost models to study how a migration controller behaves
 * under discrete page granularity.
 */
#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "policy.h"
#include <iostream>
#include <array>
#include <fstream>
#include <limits>
#include <cmath>

// Globals required by helper.cpp linkage in this repo
Helper helper{};
CXLController* controller = nullptr;
Monitors* monitors = nullptr;

struct TrialResult {
    double target_fraction;
    double final_fraction;
    size_t local_pages;
    size_t remote_pages;
    size_t moved_pages;
    double cost;
    bool pass;
};

struct ScenarioSummary {
    double m;
    double k;
    double alpha;
    double migration_cost_per_page;
    double local_access_cost;
    double remote_access_cost;
    double total_accesses;
    double model_target;
    double model_final_fraction;
    double model_cost;
    double best_target;
    double best_final_fraction;
    double best_cost;
    double cost_gap;
};

struct InitialPlacementResult {
    std::string placement;
    double target_fraction;
    double final_fraction;
    size_t moved_pages;
    double cost;
};

struct HotnessTrialResult {
    std::string policy_name;
    size_t high_value_local;
    size_t low_value_local;
    double cost;
};

enum class InitialPlacement {
    AllRemote,
    AllLocal,
    HalfLocalHalfRemote
};

static const char* initial_placement_name(InitialPlacement p) {
    switch (p) {
        case InitialPlacement::AllRemote: return "all_remote";
        case InitialPlacement::AllLocal: return "all_local";
        case InitialPlacement::HalfLocalHalfRemote: return "half_local_half_remote";
    }
    return "unknown";
}

static void print_counts(CXLController& ctrl, CXLMemExpander& exp, const char* tag) {
    const size_t local_pages = ctrl.occupation.size();
    const size_t remote_pages = exp.occupation.size();
    const size_t total_pages = local_pages + remote_pages;
    const double frac = total_pages == 0
        ? 0.0
        : static_cast<double>(local_pages) / static_cast<double>(total_pages);

    std::cout << "[" << tag << "] local_pages=" << local_pages
              << " remote_pages=" << remote_pages
              << " local_fraction=" << frac
              << std::endl;
}

static double local_fraction(CXLController& ctrl, CXLMemExpander& exp) {
    const size_t local_pages = ctrl.occupation.size();
    const size_t remote_pages = exp.occupation.size();
    const size_t total_pages = local_pages + remote_pages;
    if (total_pages == 0) return 0.0;
    return static_cast<double>(local_pages) / static_cast<double>(total_pages);
}

static double clamp01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

// Simplified model-driven target fraction.
// For now, this is intentionally lightweight: it gives us a tunable,
// paper-inspired way to map "movement benefit" and "movement cost"
// into a desired local-memory fraction.
//
// m: benefit slope of moving data local (higher means moving helps more)
// k: movement cost factor (higher means moving costs more)
// alpha: maximum allowed local fraction
static double compute_model_target_fraction(double m, double k, double alpha) {
    if (m <= 0.0 || k <= 0.0 || alpha <= 0.0) {
        return 0.0;
    }

    // Simple paper-inspired interior optimum:
    // bigger m -> move more
    // bigger k -> move less
    double x = (std::sqrt(m * k) - 1.0) / m;

    return clamp01(std::min(alpha, x));
}

static double compute_total_cost(const TrialResult& r,
                                 double migration_cost_per_page,
                                 double local_access_cost,
                                 double remote_access_cost,
                                 double total_accesses) {
    const double moved_pages = static_cast<double>(r.local_pages);
    const double total_pages = static_cast<double>(r.local_pages + r.remote_pages);

    if (total_pages == 0.0) {
        return 0.0;
    }

    const double local_frac = static_cast<double>(r.local_pages) / total_pages;
    const double remote_frac = static_cast<double>(r.remote_pages) / total_pages;

    // Nonlinear movement penalty: moving more pages gets increasingly expensive.
    // This is a simple way to create the same qualitative tradeoff as the paper.
    const double movement_cost =
        migration_cost_per_page * moved_pages * (1.0 + local_frac);

    const double execution_cost =
        total_accesses * (local_frac * local_access_cost + remote_frac * remote_access_cost);

    return movement_cost + execution_cost;
}

static double compute_total_cost_with_explicit_movement(size_t moved_pages,
                                                        size_t local_pages,
                                                        size_t remote_pages,
                                                        double migration_cost_per_page,
                                                        double local_access_cost,
                                                        double remote_access_cost,
                                                        double total_accesses) {
    const double total_pages = static_cast<double>(local_pages + remote_pages);
    if (total_pages == 0.0) {
        return 0.0;
    }

    const double local_frac = static_cast<double>(local_pages) / total_pages;
    const double remote_frac = static_cast<double>(remote_pages) / total_pages;

    const double movement_cost =
        migration_cost_per_page * static_cast<double>(moved_pages) * (1.0 + local_frac);

    const double execution_cost =
        total_accesses * (local_frac * local_access_cost + remote_frac * remote_access_cost);

    return movement_cost + execution_cost;
}

static void write_results_csv(const std::string& filename,
                              const std::vector<TrialResult>& sweep_results,
                              const TrialResult& model_result,
                              double best_target_fraction) {
    std::ofstream out(filename);
    if (!out) {
        std::cerr << "Failed to open CSV file for writing: " << filename << std::endl;
        return;
    }

    out << "target_fraction,final_fraction,cost,is_model_target,is_best_target\n";

    for (const auto& r : sweep_results) {
        out << r.target_fraction << ","
            << r.final_fraction << ","
            << r.cost << ","
            << 0 << ","
            << ((std::abs(r.target_fraction - best_target_fraction) < 1e-9) ? 1 : 0)
            << "\n";
    }

    out << model_result.target_fraction << ","
        << model_result.final_fraction << ","
        << model_result.cost << ","
        << 1 << ","
        << ((std::abs(model_result.final_fraction - best_target_fraction) < 1e-9) ? 1 : 0)
        << "\n";

    out.close();
    std::cout << "Wrote CSV results to " << filename << std::endl;
}

static void write_hotness_comparison_csv(const std::string& filename,
                                         const std::vector<HotnessTrialResult>& results) {
    std::ofstream out(filename);
    if (!out) {
        std::cerr << "Failed to open hotness comparison CSV: " << filename << std::endl;
        return;
    }

    out << "policy,high_value_local,low_value_local,cost\n";
    for (const auto& r : results) {
        out << r.policy_name << ","
            << r.high_value_local << ","
            << r.low_value_local << ","
            << r.cost << "\n";
    }

    std::cout << "Wrote hotness comparison CSV to " << filename << std::endl;
}

static void write_initial_placement_csv(const std::string& filename,
                                        const std::vector<InitialPlacementResult>& results) {
    std::ofstream out(filename);
    if (!out) {
        std::cerr << "Failed to open initial placement CSV: " << filename << std::endl;
        return;
    }

    out << "placement,target_fraction,final_fraction,moved_pages,cost\n";
    for (const auto& r : results) {
        out << r.placement << ","
            << r.target_fraction << ","
            << r.final_fraction << ","
            << r.moved_pages << ","
            << r.cost << "\n";
    }

    std::cout << "Wrote initial placement CSV to " << filename << std::endl;
}

static void append_scenario_summary_csv(const std::string& filename,
                                        const ScenarioSummary& s,
                                        bool write_header = false) {
    std::ofstream out;
    if (write_header) {
        out.open(filename, std::ios::out);
        if (!out) {
            std::cerr << "Failed to open scenario summary CSV: " << filename << std::endl;
            return;
        }
        out << "m,k,alpha,migration_cost_per_page,local_access_cost,remote_access_cost,total_accesses,"
       "model_target,model_final_fraction,model_cost,"
       "best_target,best_final_fraction,best_cost,cost_gap\n";
    } else {
        out.open(filename, std::ios::app);
        if (!out) {
            std::cerr << "Failed to append scenario summary CSV: " << filename << std::endl;
            return;
        }
    }

    out << s.m << ","
        << s.k << ","
        << s.alpha << ","
        << s.migration_cost_per_page << ","
        << s.local_access_cost << ","
        << s.remote_access_cost << ","
        << s.total_accesses << ","
        << s.model_target << ","
        << s.model_final_fraction << ","
        << s.model_cost << ","
        << s.best_target << ","
        << s.best_final_fraction << ","
        << s.best_cost << ","
        << s.cost_gap << "\n";
}

static TrialResult run_convergence_trial(double target_fraction) {
    std::array<Policy*, 4> policies = {
        new AllocationPolicy(),
        new FractionGuidedMigrationPolicy(target_fraction, 0.05, 8),
        new PagingPolicy(),
        new CachingPolicy()
    };

    CXLController ctrl(policies, 256, page_type::PAGE, 10, 100.0);
    controller = &ctrl;

    auto* exp = new CXLMemExpander(
        /*read_bw*/ 20000,
        /*write_bw*/ 20000,
        /*read_lat*/ 200,
        /*write_lat*/ 250,
        /*id*/ 0,
        /*capacity MB*/ 256
    );

    ctrl.insert_end_point(exp);
    ctrl.expanders.push_back(exp);
    ctrl.device_map[0] = exp;
    ctrl.num_end_points = 1;

    const uint64_t base = 0x100000000ULL;
    const int num_pages = 64;

    // Seed all pages into remote memory
    for (int i = 0; i < num_pages; i++) {
        occupation_info info{};
        info.timestamp = i;
        info.address = base + i * 4096;
        info.access_count = 0;
        exp->occupation.push_back(info);
    }

    auto* fgmp = dynamic_cast<FractionGuidedMigrationPolicy*>(policies[1]);
    if (!fgmp) {
        std::cerr << "FGMP cast failed" << std::endl;
        for (auto* p : policies) delete p;
        delete exp;
        return TrialResult{target_fraction, 0.0, 0, 0, 0, 0.0, false};
    }

    std::cout << "=== Convergence Test (target=" << target_fraction << ") ===" << std::endl;
    print_counts(ctrl, *exp, "initial");

    for (int round = 1; round <= 8; round++) {
        // generate the set of hot pages. The first 8
        // pages get 200 accesses
        for (int rep = 0; rep < 200; rep++) {
            for (int i = 0; i < 8; i++) {
                uint64_t hot_addr = base + i * 4096;
                fgmp->record_access(hot_addr);
            }
        }

        std::cout << "--- round " << round << " ---" << std::endl;
        print_counts(ctrl, *exp, "before");

        // is there anything to migrate
        if (fgmp->compute_once(&ctrl) > 0) {
            ctrl.perform_migration();
        } else {
            std::cout << "[round " << round << "] no migration triggered" << std::endl;
        }

        print_counts(ctrl, *exp, "after");
    }

    double final_frac = local_fraction(ctrl, *exp);
    std::cout << "Final local fraction: " << final_frac << std::endl;

    bool pass = std::abs(final_frac - target_fraction) <= 0.10;
    std::cout << (pass ? "[PASS]" : "[FAIL]")
              << " Target " << target_fraction
              << " final fraction " << final_frac << std::endl;

    size_t moved_pages = ctrl.occupation.size(); // current harness starts all remote
    TrialResult result {
        target_fraction,
        final_frac,
        ctrl.occupation.size(),
        exp->occupation.size(),
        moved_pages,
        0.0, // cost filled in later
        pass
    };

    for (auto* p : policies) delete p;
    delete exp;
    controller = nullptr;

    return result;
}

static TrialResult run_convergence_trial_with_initial_placement(double target_fraction,
                                                                InitialPlacement placement) {
    std::array<Policy*, 4> policies = {
        new AllocationPolicy(),
        new FractionGuidedMigrationPolicy(target_fraction, 0.05, 8),
        new PagingPolicy(),
        new CachingPolicy()
    };

    CXLController ctrl(policies, 256, page_type::PAGE, 10, 100.0);
    controller = &ctrl;

    auto* exp = new CXLMemExpander(
        /*read_bw*/ 20000,
        /*write_bw*/ 20000,
        /*read_lat*/ 200,
        /*write_lat*/ 250,
        /*id*/ 0,
        /*capacity MB*/ 256
    );

    ctrl.insert_end_point(exp);
    ctrl.expanders.push_back(exp);
    ctrl.device_map[0] = exp;
    ctrl.num_end_points = 1;

    const uint64_t base = 0x400000000ULL;
    const int num_pages = 64;

    // Seed pages according to the selected initial placement.
    for (int i = 0; i < num_pages; i++) {
        occupation_info info{};
        info.timestamp = i;
        info.address = base + static_cast<uint64_t>(i) * 4096;
        info.access_count = 0;

        switch (placement) {
            case InitialPlacement::AllRemote:
                exp->occupation.push_back(info);
                break;
            case InitialPlacement::AllLocal:
                ctrl.occupation.emplace(i, info);
                break;
            case InitialPlacement::HalfLocalHalfRemote:
                if (i < num_pages / 2) {
                    ctrl.occupation.emplace(i, info);
                } else {
                    exp->occupation.push_back(info);
                }
                break;
        }
    }

    auto* fgmp = dynamic_cast<FractionGuidedMigrationPolicy*>(policies[1]);
    if (!fgmp) {
        std::cerr << "FGMP cast failed in initial placement test" << std::endl;
        for (auto* p : policies) delete p;
        delete exp;
        controller = nullptr;
        return TrialResult{target_fraction, 0.0, 0, 0, 0, 0.0, false};
    }

    std::cout << "=== Initial Placement Trial (" << initial_placement_name(placement)
              << ", target=" << target_fraction << ") ===" << std::endl;
    print_counts(ctrl, *exp, "initial");

    const size_t initial_local = ctrl.occupation.size();

    for (int round = 1; round <= 8; round++) {
        // Keep the same synthetic "hot subset" used by the main harness.
        for (int rep = 0; rep < 200; rep++) {
            for (int i = 0; i < 8; i++) {
                uint64_t hot_addr = base + static_cast<uint64_t>(i) * 4096;
                fgmp->record_access(hot_addr);
            }
        }

        if (fgmp->compute_once(&ctrl) > 0) {
            ctrl.perform_migration();
        } else {
            break;
        }
    }

    print_counts(ctrl, *exp, "final");

    const double final_frac = local_fraction(ctrl, *exp);
    const bool pass = std::abs(final_frac - target_fraction) <= 0.10;

    size_t final_local = ctrl.occupation.size();
    size_t moved_pages = (final_local > initial_local)
        ? (final_local - initial_local)
        : (initial_local - final_local);

    TrialResult result{
        target_fraction,
        final_frac,
        ctrl.occupation.size(),
        exp->occupation.size(),
        moved_pages,
        0.0,
        pass
    };

    for (auto* p : policies) delete p;
    delete exp;
    controller = nullptr;

    return result;
}

static TrialResult run_model_driven_trial(double m, double k, double alpha) {
    double target = compute_model_target_fraction(m, k, alpha);

    std::cout << "=== Model-Driven Trial ===" << std::endl;
    std::cout << "m=" << m
              << " k=" << k
              << " alpha=" << alpha
              << " -> target=" << target
              << std::endl;

    return run_convergence_trial(target);
}

static ScenarioSummary run_best_target_comparison(
    double m,
    double k,
    double alpha,
    double migration_cost_per_page,
    double local_access_cost,
    double remote_access_cost,
    double total_accesses) {
    std::cout << "=== Best-Target Comparison ===" << std::endl;
    std::cout << "m=" << m << " k=" << k << " alpha=" << alpha << std::endl;

    // Candidate fractions reachable under the current discrete setup
    std::vector<double> candidates = {0.125, 0.25, 0.375, 0.5, 0.625, 0.75};

    // Sweep all candidates
    double best_cost = std::numeric_limits<double>::max();
    TrialResult best_result{};
    bool best_set = false;
    std::vector<TrialResult> sweep_results;

    for (double target : candidates) {
        TrialResult r = run_convergence_trial(target);
        r.cost = compute_total_cost(
            r,
            migration_cost_per_page,
            local_access_cost,
            remote_access_cost,
            total_accesses
        );

        std::cout << "[sweep] target=" << target
                  << " final=" << r.final_fraction
                  << " cost=" << r.cost
                  << std::endl;

        if (!best_set || r.cost < best_cost) {
            best_cost = r.cost;
            best_result = r;
            best_set = true;
        }

        sweep_results.push_back(r);
        std::cout << std::endl;
    }

    // Model-driven target
    double model_target = compute_model_target_fraction(m, k, alpha);
    TrialResult model_result = run_convergence_trial(model_target);
    model_result.cost = compute_total_cost(
        model_result,
        migration_cost_per_page,
        local_access_cost,
        remote_access_cost,
        total_accesses
    );

    std::cout << "[model] target=" << model_target
              << " final=" << model_result.final_fraction
              << " cost=" << model_result.cost
              << std::endl;

    std::cout << "[best] target=" << best_result.target_fraction
              << " final=" << best_result.final_fraction
              << " cost=" << best_cost
              << std::endl;

    double gap = model_result.cost - best_cost;
    std::cout << "Model-to-best cost gap: " << gap << std::endl;

    write_results_csv("fraction_guided_results.csv",
        sweep_results,
        model_result,
        best_result.target_fraction);

    ScenarioSummary summary{
        m,
        k,
        alpha,
        migration_cost_per_page,
        local_access_cost,
        remote_access_cost,
        total_accesses,
        model_target,
        model_result.final_fraction,
        model_result.cost,
        best_result.target_fraction,
        best_result.final_fraction,
        best_cost,
        gap
    };

    return summary;
}

static size_t count_local_pages_in_range(CXLController& ctrl,
    uint64_t base,
    int start_page,
    int num_pages) {
    uint64_t start_addr = base + static_cast<uint64_t>(start_page) * 4096;
    uint64_t end_addr = base + static_cast<uint64_t>(start_page + num_pages) * 4096;

    size_t count = 0;
    for (const auto& [ts, info] : ctrl.occupation) {
        if (info.address >= start_addr && info.address < end_addr) {
            count++;
        }
    }
    return count;
}

static double compute_importance_aware_trial_cost(CXLController& ctrl, uint64_t base) {
    // Pages 0-7 are "high-importance"
    // Pages 8-15 are "low-importance"
    //
    // Lower cost is better.
    // Keeping a high-importance page remote is much more expensive.

    double cost = 0.0;

    for (int i = 0; i < 16; i++) {
        uint64_t addr = base + static_cast<uint64_t>(i) * 4096;
        bool is_local = false;

        for (const auto& [ts, info] : ctrl.occupation) {
            if (info.address == addr) {
                is_local = true;
                break;
            }
        }

        bool high_importance = (i < 8);

        if (high_importance) {
            cost += is_local ? 1.0 : 5.0;
        } else {
            cost += is_local ? 1.0 : 2.0;
        }
    }

    return cost;
}

static HotnessTrialResult run_frequency_only_trial() {
    std::array<Policy*, 4> policies = {
        new AllocationPolicy(),
        new FractionGuidedMigrationPolicy(0.5, 0.05, 8),
        new PagingPolicy(),
        new CachingPolicy()
    };

    CXLController ctrl(policies, 256, page_type::PAGE, 10, 100.0);
    controller = &ctrl;

    auto* exp = new CXLMemExpander(
        /*read_bw*/ 20000,
        /*write_bw*/ 20000,
        /*read_lat*/ 200,
        /*write_lat*/ 250,
        /*id*/ 0,
        /*capacity MB*/ 256
    );

    ctrl.insert_end_point(exp);
    ctrl.expanders.push_back(exp);
    ctrl.device_map[0] = exp;
    ctrl.num_end_points = 1;

    const uint64_t base = 0x300000000ULL;
    const int num_pages = 16;

    for (int i = 0; i < num_pages; i++) {
        occupation_info info{};
        info.timestamp = i;
        info.address = base + static_cast<uint64_t>(i) * 4096;
        info.access_count = 0;
        exp->occupation.push_back(info);
    }

    auto* fgmp = dynamic_cast<FractionGuidedMigrationPolicy*>(policies[1]);
    if (!fgmp) {
        std::cerr << "FGMP cast failed in plain hotness test" << std::endl;
        for (auto* p : policies) delete p;
        delete exp;
        controller = nullptr;
        return HotnessTrialResult{"frequency-only", 0, 0, 0.0};
    }

    std::cout << "=== Frequency-Only Test ===" << std::endl;
    print_counts(ctrl, *exp, "initial");

    // Low-importance pages are slightly "hotter" by raw count.
    // This should make plain hotness prefer them.
    for (int rep = 0; rep < 100; rep++) {
        for (int i = 0; i < 8; i++) {
            fgmp->record_access(base + static_cast<uint64_t>(i) * 4096, 1.0);      // high-importance group
            fgmp->record_access(base + static_cast<uint64_t>(8 + i) * 4096, 2.0);  // low-importance group appears hotter
        }
    }

    if (fgmp->compute_once(&ctrl) > 0) {
        ctrl.perform_migration();
    }

    print_counts(ctrl, *exp, "after");

    size_t high_local = count_local_pages_in_range(ctrl, base, 0, 8);
    size_t low_local = count_local_pages_in_range(ctrl, base, 8, 8);
    double cost = compute_importance_aware_trial_cost(ctrl, base);

    std::cout << "High-importance local pages: " << high_local << std::endl;
    std::cout << "Low-importance local pages: " << low_local << std::endl;
    std::cout << "Plain-hotness cost: " << cost << std::endl;

    HotnessTrialResult result{
        "frequency-only",
        high_local,
        low_local,
        cost
    };

    for (auto* p : policies) delete p;
    delete exp;
    controller = nullptr;

    return result;
}

static HotnessTrialResult run_weighted_hotness_trial() {
    std::array<Policy*, 4> policies = {
        new AllocationPolicy(),
        new FractionGuidedMigrationPolicy(0.125, 0.05, 8),
        new PagingPolicy(),
        new CachingPolicy()
    };

    CXLController ctrl(policies, 256, page_type::PAGE, 10, 100.0);
    controller = &ctrl;

    auto* exp = new CXLMemExpander(
        /*read_bw*/ 20000,
        /*write_bw*/ 20000,
        /*read_lat*/ 200,
        /*write_lat*/ 250,
        /*id*/ 0,
        /*capacity MB*/ 256
    );

    ctrl.insert_end_point(exp);
    ctrl.expanders.push_back(exp);
    ctrl.device_map[0] = exp;
    ctrl.num_end_points = 1;

    const uint64_t base = 0x200000000ULL;
    const int num_pages = 16;

    // Seed 16 remote pages.
    for (int i = 0; i < num_pages; i++) {
        occupation_info info{};
        info.timestamp = i;
        info.address = base + static_cast<uint64_t>(i) * 4096;
        info.access_count = 0;
        exp->occupation.push_back(info);
    }

    auto* fgmp = dynamic_cast<FractionGuidedMigrationPolicy*>(policies[1]);
    if (!fgmp) {
        std::cerr << "FGMP cast failed in weighted test" << std::endl;
        for (auto* p : policies) delete p;
        delete exp;
        controller = nullptr;
        return HotnessTrialResult{"weighted_hotness", 0, 0, 0.0};
    }

    std::cout << "=== Weighted Hotness Test ===" << std::endl;
    print_counts(ctrl, *exp, "initial");

    // Low-importance pages are touched more often,
    // but high-importance pages receive a larger weight.
    // Weighted hotness should therefore recover the important set.
    for (int rep = 0; rep < 100; rep++) {
        for (int i = 0; i < 8; i++) {
            fgmp->record_access(base + static_cast<uint64_t>(i) * 4096, 4.0);      // high importance
            fgmp->record_access(base + static_cast<uint64_t>(8 + i) * 4096, 2.0);  // low importance but hotter by count
        }
    }

    if (fgmp->compute_once(&ctrl) > 0) {
        ctrl.perform_migration();
    }

    print_counts(ctrl, *exp, "after");

    size_t high_local = count_local_pages_in_range(ctrl, base, 0, 8);
    size_t low_local = count_local_pages_in_range(ctrl, base, 8, 8);

    std::cout << "High-importance local pages: " << high_local << std::endl;
    std::cout << "Low-importance local pages: " << low_local << std::endl;

    double cost = compute_importance_aware_trial_cost(ctrl, base);
    std::cout << "Weighted-hotness cost: " << cost << std::endl;

    bool pass = (high_local == 8 && low_local == 0);
    std::cout << (pass ? "[PASS]" : "[FAIL]")
              << " Weighted hotness prioritization" << std::endl; 

    HotnessTrialResult result{
        "weighted_hotness",
        high_local,
        low_local,
        cost
    };

    for (auto* p : policies) delete p;
    delete exp;
    controller = nullptr;

    return result;
}

static void run_initial_placement_sensitivity() {
    std::cout << "=== Initial Placement Sensitivity ===" << std::endl;

    const double target_fraction = 0.5;

    // Use the same "middle-regime" cost model as the main matched case.
    const double migration_cost_per_page = 15.0;
    const double local_access_cost = 1.0;
    const double remote_access_cost = 3.0;
    const double total_accesses = 1000.0;

    std::vector<InitialPlacement> placements = {
        InitialPlacement::AllRemote,
        InitialPlacement::AllLocal,
        InitialPlacement::HalfLocalHalfRemote
    };

    std::vector<InitialPlacementResult> results;

    for (auto placement : placements) {
        TrialResult r = run_convergence_trial_with_initial_placement(target_fraction, placement);
        r.cost = compute_total_cost_with_explicit_movement(
            r.moved_pages,
            r.local_pages,
            r.remote_pages,
            migration_cost_per_page,
            local_access_cost,
            remote_access_cost,
            total_accesses
        );

        std::cout << "[initial-placement] " << initial_placement_name(placement)
                  << " final=" << r.final_fraction
                  << " moved=" << r.moved_pages
                  << " cost=" << r.cost
                  << std::endl;

        results.push_back(InitialPlacementResult{
            initial_placement_name(placement),
            target_fraction,
            r.final_fraction,
            r.moved_pages,
            r.cost
        });

        std::cout << std::endl;
    }

    write_initial_placement_csv("initial_placement_sensitivity.csv", results);
    std::cout << std::endl;
}


// TODO: add results of hotness aware policy to CSV that compares methods and interpret results
int main() {
    // 1. Page-selection experiment: frequency-only vs weighted hotness
    // 2. Fixed-target convergence sweep
    // 3. Model-driven target sweep
    // 4. Best-target comparison across scenarios
    HotnessTrialResult plain_result = run_frequency_only_trial();
    std::cout << std::endl;

    HotnessTrialResult weighted_result = run_weighted_hotness_trial();
    std::cout << std::endl;

    write_hotness_comparison_csv("hotness_comparison.csv", {plain_result, weighted_result});
    std::cout << std::endl;

    run_initial_placement_sensitivity();

    {
        std::vector<double> targets = {0.25, 0.50, 0.75};

        int passed = 0;
        int failed = 0;

        for (double target : targets) {
            TrialResult result = run_convergence_trial(target);
            if (result.pass) {
                passed++;
            } else {
                failed++;
            }
            std::cout << std::endl;
        }

        std::cout << "=== Fixed-Target Sweep Summary ===" << std::endl;
        std::cout << "Passed: " << passed << std::endl;
        std::cout << "Failed: " << failed << std::endl;
        std::cout << std::endl;
    }

    {
        struct ModelCase {
            double m;
            double k;
            double alpha;
        };

        std::vector<ModelCase> cases = {
            {2.0, 2.0, 1.0},
            {4.0, 2.0, 1.0},
            {4.0, 1.2, 1.0},
            {4.0, 2.0, 0.5}
        };

        int passed = 0;
        int failed = 0;

        for (const auto& c : cases) {
            TrialResult result = run_model_driven_trial(c.m, c.k, c.alpha);
            if (result.pass) {
                passed++;
            } else {
                failed++;
            }
            std::cout << std::endl;
        }

        std::cout << "=== Model-Driven Sweep Summary ===" << std::endl;
        std::cout << "Passed: " << passed << std::endl;
        std::cout << "Failed: " << failed << std::endl;

        std::cout << std::endl;

        struct ComparisonCase {
            double m;
            double k;
            double alpha;
            double migration_cost_per_page;
            double local_access_cost;
            double remote_access_cost;
            double total_accesses;
        };

        std::vector<ComparisonCase> comparison_cases = {
            // ----- Middle-regime cost model -----
            {4.0, 2.0, 1.0, 15.0, 1.0, 3.0, 1000.0},
            {2.0, 2.0, 1.0, 15.0, 1.0, 3.0, 1000.0},
            {4.0, 1.2, 1.0, 15.0, 1.0, 3.0, 1000.0},

            // ----- Lower-fraction-favoring cost model -----
            {4.0, 2.0, 1.0, 22.0, 1.0, 2.0, 1000.0},
            {2.0, 2.0, 1.0, 22.0, 1.0, 2.0, 1000.0},
            {4.0, 1.2, 1.0, 22.0, 1.0, 2.0, 1000.0},

            // ----- Higher-fraction-favoring cost model -----
            {4.0, 2.0, 1.0, 8.0, 1.0, 4.0, 1000.0},
            {2.0, 2.0, 1.0, 8.0, 1.0, 4.0, 1000.0},
            {4.0, 1.2, 1.0, 8.0, 1.0, 4.0, 1000.0}
        };

        const std::string summary_csv = "fraction_guided_summary.csv";
        bool first = true;

        for (const auto& c : comparison_cases) {
            ScenarioSummary s = run_best_target_comparison(
                c.m,
                c.k,
                c.alpha,
                c.migration_cost_per_page,
                c.local_access_cost,
                c.remote_access_cost,
                c.total_accesses
            );
            append_scenario_summary_csv(summary_csv, s, first);
            first = false;
            std::cout << std::endl;
        }

        std::cout << "Wrote scenario summary CSV to " << summary_csv << std::endl;
        return failed == 0 ? 0 : 1;
    }
}
