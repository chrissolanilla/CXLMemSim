from pathlib import Path
import csv

import matplotlib.pyplot as plt


def main() -> None:
    csv_path = Path("build/fraction_guided_results.csv")
    if not csv_path.exists():
        raise FileNotFoundError(f"CSV not found: {csv_path}")

    rows = []
    with csv_path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                {
                    "target_fraction": float(row["target_fraction"]),
                    "final_fraction": float(row["final_fraction"]),
                    "cost": float(row["cost"]),
                    "is_model_target": int(row["is_model_target"]),
                    "is_best_target": int(row["is_best_target"]),
                }
            )

    sweep_rows = [r for r in rows if r["is_model_target"] == 0]
    model_rows = [r for r in rows if r["is_model_target"] == 1]

    if len(model_rows) != 1:
        raise ValueError(f"Expected exactly 1 model row, found {len(model_rows)}")

    model = model_rows[0]

    sweep_rows.sort(key=lambda r: r["final_fraction"])

    x = [r["final_fraction"] for r in sweep_rows]
    y = [r["cost"] for r in sweep_rows]

    plt.figure(figsize=(8, 5))
    plt.plot(x, y, marker="o", label="Sweep targets")

    best_rows = [r for r in sweep_rows if r["is_best_target"] == 1]
    if best_rows:
        best = best_rows[0]
        plt.scatter(
            [best["final_fraction"]],
            [best["cost"]],
            s=100,
            marker="*",
            label="Best target",
        )

    plt.scatter(
        [model["final_fraction"]],
        [model["cost"]],
        s=80,
        marker="s",
        label="Model-driven target",
    )

    plt.xlabel("Final local fraction")
    plt.ylabel("Synthetic total cost")
    plt.title("Fraction-guided migration: sweep vs model")
    plt.grid(True, alpha=0.3)
    plt.legend()

    out_path = Path("build/fraction_guided_results.png")
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    print(f"Wrote plot to {out_path}")


if __name__ == "__main__":
    main()
