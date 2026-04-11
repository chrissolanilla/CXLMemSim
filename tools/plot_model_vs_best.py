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

    model_final = [r["model_final_fraction"] for r in rows]
    best_final = [r["best_final_fraction"] for r in rows]
    model_target = [r["model_target"] for r in rows]
    gaps = [r["cost_gap"] for r in rows]

    # ---- Figure 1: model target vs realized/best fraction ----
    plt.figure(figsize=(8, 4.8))

    plt.plot(x, model_target, marker="o", linestyle="--", label="Model target")
    plt.plot(x, model_final, marker="s", label="Model final fraction")
    plt.plot(x, best_final, marker="*", markersize=12, label="Best final fraction")

    plt.xticks(x, labels)
    plt.xlabel("Scenario (m, k, α)")
    plt.ylabel("Local-memory fraction")
    plt.title("Model-selected vs best fraction across scenarios")
    plt.ylim(0, 1.0)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()

    out1 = Path("build/model_vs_best_fraction.png")
    plt.savefig(out1, dpi=200)
    plt.close()

    # ---- Figure 2: model-to-best cost gap ----
    plt.figure(figsize=(8, 4.2))

    plt.bar(x, gaps)
    plt.xticks(x, labels)
    plt.xlabel("Scenario (m, k, α)")
    plt.ylabel("Cost gap")
    plt.title("Model-to-best cost gap across scenarios")
    plt.grid(True, axis="y", alpha=0.3)
    plt.tight_layout()

    out2 = Path("build/model_vs_best_gap.png")
    plt.savefig(out2, dpi=200)
    plt.close()

    print(f"Wrote {out1}")
    print(f"Wrote {out2}")


if __name__ == "__main__":
    main()
