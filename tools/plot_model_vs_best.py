from pathlib import Path
import csv

import matplotlib.pyplot as plt


def read_summary_csv(path: Path) -> list[dict]:
    rows = []
    with path.open(newline="") as f:
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
    return rows


def main() -> None:
    csv_path = Path("build/fraction_guided_summary.csv")
    if not csv_path.exists():
        raise FileNotFoundError(f"Could not find {csv_path}")

    rows = read_summary_csv(csv_path)
    if not rows:
        raise ValueError("Summary CSV is empty")

    labels = [f"({int(r['m'])},{r['k']},{r['alpha']})" for r in rows]
    x = list(range(len(rows)))

    gaps = [r["cost_gap"] for r in rows]

    plt.figure(figsize=(8, 4.2))

    plt.bar(x, gaps)
    plt.xticks(x, labels)
    plt.xlabel("Scenario (m, k, α)")
    plt.ylabel("Cost gap")
    plt.title("Model-to-best cost gap across scenarios")
    plt.grid(True, axis="y", alpha=0.3)
    plt.tight_layout()

    out = Path("plots/model_vs_best_gap.png")
    plt.savefig(out, dpi=200)
    plt.close()

    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
