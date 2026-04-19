3

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


INPUT_CSV = "build/policy_sweep_full.csv"
OUTPUT_PNG = "plots/policy_barplot_representative_lambda.png"

# Keep one representative scenario fixed.
FILTERS = {
    "num_pages": 64,
    "hot_pages": 8,
    "batch_size": 8,
    "alpha": 1.0,
}

POLICY_ORDER = [
    "no_migration",
    "full_migration",
    "naive_hotness",
    "paper_fraction_guided",
]

POLICY_LABELS = {
    "no_migration": "no migration",
    "full_migration": "full migration",
    "naive_hotness": "naive hotness",
    "paper_fraction_guided": "paper fraction guided",
}

# Target lambda values for representative case studies.
# The script will choose the closest available scenario to each target.
TARGET_LAMBDAS = [0.75, 1.00, 2.50, 4.00, 7.00]


def float_equal(a: float, b: float, eps: float = 1e-9) -> bool:
    return abs(a - b) < eps


def parse_row(row: dict[str, str]) -> dict[str, object]:
    return {
        "num_pages": int(row["num_pages"]),
        "hot_pages": int(row["hot_pages"]),
        "batch_size": int(row["batch_size"]),
        "alpha": float(row["alpha"]),
        "k": float(row["k"]),
        "m": float(row["m"]),
        "policy": row["policy"],
        "cost2_policy": float(row["cost2_policy"]),
    }


def row_matches_filters(row: dict[str, object]) -> bool:
    if row["num_pages"] != FILTERS["num_pages"]:
        return False
    if row["hot_pages"] != FILTERS["hot_pages"]:
        return False
    if row["batch_size"] != FILTERS["batch_size"]:
        return False
    if not float_equal(float(row["alpha"]), FILTERS["alpha"]):
        return False
    return True


def load_filtered_rows(csv_path: str) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []

    with open(csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)

        required_cols = {
            "num_pages",
            "hot_pages",
            "batch_size",
            "alpha",
            "k",
            "m",
            "policy",
            "cost2_policy",
        }
        missing = required_cols - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"Missing required columns in CSV: {sorted(missing)}")

        for raw_row in reader:
            row = parse_row(raw_row)
            if row_matches_filters(row):
                rows.append(row)

    if not rows:
        raise ValueError(
            "Filtering removed all rows. Adjust FILTERS to match your CSV."
        )

    return rows


def normalize_rows(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    """
    Normalize each policy cost against no_migration for the same scenario.
    Scenario key is fixed-filter dimensions plus (k, m).
    """
    baseline_by_key: dict[tuple, float] = {}

    for row in rows:
        key = (
            row["num_pages"],
            row["hot_pages"],
            row["batch_size"],
            round(float(row["alpha"]), 12),
            round(float(row["k"]), 12),
            round(float(row["m"]), 12),
        )
        if row["policy"] == "no_migration":
            baseline_by_key[key] = float(row["cost2_policy"])

    if not baseline_by_key:
        raise ValueError("No no_migration rows found after filtering.")

    normalized: list[dict[str, object]] = []

    for row in rows:
        key = (
            row["num_pages"],
            row["hot_pages"],
            row["batch_size"],
            round(float(row["alpha"]), 12),
            round(float(row["k"]), 12),
            round(float(row["m"]), 12),
        )

        if key not in baseline_by_key:
            raise ValueError(f"No no_migration baseline found for scenario key: {key}")

        baseline_cost = baseline_by_key[key]
        policy_cost = float(row["cost2_policy"])

        normalized.append(
            {
                "k": float(row["k"]),
                "m": float(row["m"]),
                "lambda": float(row["k"]) * float(row["m"]),
                "policy": str(row["policy"]),
                "normalized_cost": policy_cost / baseline_cost,
            }
        )

    return normalized


def build_scenario_table(
    rows: list[dict[str, object]],
) -> dict[tuple[float, float], dict[str, float]]:
    """
    Build a table:
        (k, m) -> {
            "lambda": ...,
            "no_migration": ...,
            "full_migration": ...,
            ...
        }

    If duplicates exist, average normalized costs per policy.
    """
    grouped: dict[tuple[float, float], dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )

    for row in rows:
        k = round(float(row["k"]), 12)
        m = round(float(row["m"]), 12)
        policy = str(row["policy"])
        if policy not in POLICY_ORDER:
            continue
        grouped[(k, m)][policy].append(float(row["normalized_cost"]))

    scenarios: dict[tuple[float, float], dict[str, float]] = {}

    for (k, m), policy_map in grouped.items():
        scenario: dict[str, float] = {
            "k": k,
            "m": m,
            "lambda": k * m,
        }
        for policy in POLICY_ORDER:
            values = policy_map.get(policy, [])
            if not values:
                break
            scenario[policy] = sum(values) / len(values)
        else:
            scenarios[(k, m)] = scenario

    if not scenarios:
        raise ValueError("No complete scenarios found with all required policies.")

    return scenarios


def choose_representative_scenarios(
    scenarios: dict[tuple[float, float], dict[str, float]],
    target_lambdas: list[float],
) -> list[dict[str, float]]:
    """
    For each target lambda, choose the closest available scenario.
    Avoid reusing the same (k,m) scenario twice if possible.
    """
    available = list(scenarios.values())
    chosen: list[dict[str, float]] = []
    used_keys: set[tuple[float, float]] = set()

    for target in target_lambdas:
        candidates = sorted(
            available, key=lambda s: (abs(s["lambda"] - target), s["lambda"])
        )

        picked = None
        for cand in candidates:
            key = (cand["k"], cand["m"])
            if key not in used_keys:
                picked = cand
                used_keys.add(key)
                break

        if picked is None and candidates:
            picked = candidates[0]

        if picked is not None:
            chosen.append(picked)

    # Sort chosen scenarios by actual lambda for clean x-axis order
    chosen.sort(key=lambda s: s["lambda"])
    return chosen


def make_bar_plot(chosen: list[dict[str, float]], output_path: str) -> None:
    if not chosen:
        raise ValueError("No representative scenarios selected.")

    x_positions = list(range(len(chosen)))
    width = 0.18

    plt.figure(figsize=(10, 5.8))

    for idx, policy in enumerate(POLICY_ORDER):
        offsets = [x + (idx - 1.5) * width for x in x_positions]
        heights = [scenario[policy] for scenario in chosen]

        plt.bar(
            offsets,
            heights,
            width=width,
            label=POLICY_LABELS[policy],
        )

    # Tick labels show actual lambda values chosen
    tick_labels = [
        f"λ={scenario['lambda']:.2f}\n(k={scenario['k']:.2f}, m={scenario['m']:.2f})"
        for scenario in chosen
    ]

    plt.xticks(x_positions, tick_labels)
    plt.axhline(1.0, linestyle=":", linewidth=1)
    plt.text(
        -0.45,
        1.0,
        " no-migration baseline",
        ha="left",
        va="bottom",
        fontsize=10,
    )

    plt.title(
        "Representative policy comparison across λ = m × k\n"
        f"(num_pages={FILTERS['num_pages']}, hot_pages={FILTERS['hot_pages']}, "
        f"batch_size={FILTERS['batch_size']}, alpha={FILTERS['alpha']})"
    )
    plt.xlabel("Representative λ regimes")
    plt.ylabel("Normalized cost (policy cost / no_migration cost)")
    plt.legend()
    plt.grid(True, axis="y", alpha=0.3)
    plt.tight_layout()

    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(output, dpi=300, bbox_inches="tight")
    plt.close()


def print_selected_scenarios(chosen: list[dict[str, float]]) -> None:
    print("Selected representative scenarios:")
    for s in chosen:
        print(
            f"  lambda={s['lambda']:.4f}, k={s['k']:.4f}, m={s['m']:.4f}, "
            f"no_migration={s['no_migration']:.4f}, "
            f"full_migration={s['full_migration']:.4f}, "
            f"naive_hotness={s['naive_hotness']:.4f}, "
            f"paper_fraction_guided={s['paper_fraction_guided']:.4f}"
        )


def main() -> None:
    filtered_rows = load_filtered_rows(INPUT_CSV)
    normalized_rows = normalize_rows(filtered_rows)
    scenarios = build_scenario_table(normalized_rows)
    chosen = choose_representative_scenarios(scenarios, TARGET_LAMBDAS)

    if not chosen:
        raise ValueError("Could not select representative scenarios.")

    print_selected_scenarios(chosen)
    make_bar_plot(chosen, OUTPUT_PNG)
    print(f"Wrote plot to: {OUTPUT_PNG}")


if __name__ == "__main__":
    main()
