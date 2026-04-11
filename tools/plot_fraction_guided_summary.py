from pathlib import Path
import csv

import matplotlib.pyplot as plt


def main() -> None:
    csv_path = Path("build/fraction_guided_summary.csv")
    if not csv_path.exists():
        raise FileNotFoundError(f"CSV not found: {csv_path}")

    rows = []
    with csv_path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                {
                    "m": float(row["m"]),
                    "k": float(row["k"]),
                    "alpha": float(row["alpha"]),
                    "model_target": float(row["model_target"]),
                    "model_final_fraction": float(row["model_final_fraction"]),
                    "model_cost": float(row["model_cost"]),
                    "best_target": float(row["best_target"]),
                    "best_final_fraction": float(row["best_final_fraction"]),
                    "best_cost": float(row["best_cost"]),
                    "cost_gap": float(row["cost_gap"]),
                }
            )

    labels = [f"m={r['m']}, k={r['k']}, a={r['alpha']}" for r in rows]
    x = list(range(len(rows)))

    model_vals = [r["model_final_fraction"] for r in rows]
    best_vals = [r["best_final_fraction"] for r in rows]
    gaps = [r["cost_gap"] for r in rows]

    plt.figure(figsize=(10, 5))
    plt.plot(x, model_vals, marker="s", label="Model final fraction")
    plt.plot(x, best_vals, marker="*", label="Best final fraction")

    plt.xticks(x, labels, rotation=20, ha="right")
    plt.ylabel("Final local fraction")
    plt.title("Model-selected vs best sweep fraction across scenarios")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig("build/fraction_guided_summary_fractions.png", dpi=150)

    plt.figure(figsize=(10, 5))
    plt.plot(x, gaps, marker="o")
    plt.xticks(x, labels, rotation=20, ha="right")
    plt.ylabel("Model-to-best cost gap")
    plt.title("Model cost gap across scenarios")
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig("build/fraction_guided_summary_gaps.png", dpi=150)

    print(
        "Wrote plots to build/fraction_guided_summary_fractions.png and build/fraction_guided_summary_gaps.png"
    )


if __name__ == "__main__":
    main()
