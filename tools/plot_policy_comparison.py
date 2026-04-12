#!/usr/bin/env python3

import argparse
import math
import os
from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt


def require_columns(df: pd.DataFrame, required: list[str]) -> None:
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise ValueError(f"CSV is missing required columns: {missing}")


def safe_mean_table(df: pd.DataFrame, index: str, columns: str, values: str) -> pd.DataFrame:
    table = pd.pivot_table(
        df,
        index=index,
        columns=columns,
        values=values,
        aggfunc="mean",
    )
    return table.sort_index().sort_index(axis=1)


def save_fig(fig: plt.Figure, outdir: Path, name: str) -> None:
    outpath = outdir / name
    fig.tight_layout()
    fig.savefig(outpath, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {outpath}")


def plot_avg_speedup_by_policy(df: pd.DataFrame, outdir: Path) -> None:
    grouped = (
        df.groupby("policy", as_index=False)["g_of_x"]
        .mean()
        .sort_values("g_of_x", ascending=False)
    )

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.bar(grouped["policy"], grouped["g_of_x"])
    ax.set_title("Average g_of_x by policy")
    ax.set_ylabel("Average g_of_x")
    ax.set_xlabel("Policy")
    ax.tick_params(axis="x", rotation=20)
    save_fig(fig, outdir, "avg_g_of_x_by_policy.png")


def plot_param_lines(df: pd.DataFrame, outdir: Path, param: str) -> None:
    fig, ax = plt.subplots(figsize=(8, 5))

    for policy in sorted(df["policy"].unique()):
        sub = df[df["policy"] == policy]
        grouped = sub.groupby(param, as_index=False)["g_of_x"].mean().sort_values(param)
        ax.plot(grouped[param], grouped["g_of_x"], marker="o", label=policy)

    ax.set_title(f"Average g_of_x vs {param}")
    ax.set_xlabel(param)
    ax.set_ylabel("Average g_of_x")
    ax.legend()
    save_fig(fig, outdir, f"avg_g_of_x_vs_{param}.png")


def plot_fraction_tracking(df: pd.DataFrame, outdir: Path) -> None:
    if "chosen_target_fraction" not in df.columns or "final_fraction" not in df.columns:
        return

    fig, ax = plt.subplots(figsize=(7, 6))

    for policy in sorted(df["policy"].unique()):
        sub = df[df["policy"] == policy]
        ax.scatter(
            sub["chosen_target_fraction"],
            sub["final_fraction"],
            label=policy,
            alpha=0.7,
        )

    max_val = max(
        1.0,
        float(df["chosen_target_fraction"].max(skipna=True)),
        float(df["final_fraction"].max(skipna=True)),
    )
    ax.plot([0, max_val], [0, max_val], linestyle="--")
    ax.set_title("Chosen target fraction vs final realized fraction")
    ax.set_xlabel("Chosen target fraction")
    ax.set_ylabel("Final realized fraction")
    ax.legend()
    save_fig(fig, outdir, "chosen_vs_final_fraction.png")


def plot_moved_pages_vs_speedup(df: pd.DataFrame, outdir: Path) -> None:
    if "moved_pages" not in df.columns:
        return

    fig, ax = plt.subplots(figsize=(7, 6))
    for policy in sorted(df["policy"].unique()):
        sub = df[df["policy"] == policy]
        ax.scatter(sub["moved_pages"], sub["g_of_x"], label=policy, alpha=0.7)

    ax.set_title("Moved pages vs g_of_x")
    ax.set_xlabel("Moved pages")
    ax.set_ylabel("g_of_x")
    ax.legend()
    save_fig(fig, outdir, "moved_pages_vs_g_of_x.png")


def plot_heatmap_for_paper_policy(df: pd.DataFrame, outdir: Path) -> None:
    sub = df[df["policy"] == "paper_fraction_guided"].copy()
    if sub.empty:
        return

    needed = ["k", "m", "g_of_x"]
    require_columns(sub, needed)

    if sub["k"].nunique() < 2 or sub["m"].nunique() < 2:
        return

    table = safe_mean_table(sub, index="m", columns="k", values="g_of_x")
    if table.empty:
        return

    fig, ax = plt.subplots(figsize=(8, 6))
    im = ax.imshow(table.values, aspect="auto", origin="lower")

    ax.set_title("Paper-guided policy: mean g_of_x over m × k")
    ax.set_xlabel("k")
    ax.set_ylabel("m")
    ax.set_xticks(range(len(table.columns)))
    ax.set_xticklabels([f"{x:g}" for x in table.columns])
    ax.set_yticks(range(len(table.index)))
    ax.set_yticklabels([f"{x:g}" for x in table.index])

    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label("Mean g_of_x")

    save_fig(fig, outdir, "paper_policy_heatmap_m_by_k.png")


def plot_alpha_slices_for_paper_policy(df: pd.DataFrame, outdir: Path) -> None:
    sub = df[df["policy"] == "paper_fraction_guided"].copy()
    if sub.empty or "alpha" not in sub.columns:
        return

    fig, ax = plt.subplots(figsize=(8, 5))

    for alpha_val in sorted(sub["alpha"].dropna().unique()):
        alpha_df = sub[sub["alpha"] == alpha_val]
        grouped = alpha_df.groupby("k", as_index=False)["g_of_x"].mean().sort_values("k")
        ax.plot(grouped["k"], grouped["g_of_x"], marker="o", label=f"alpha={alpha_val:g}")

    ax.set_title("Paper-guided policy: g_of_x vs k for each alpha")
    ax.set_xlabel("k")
    ax.set_ylabel("Average g_of_x")
    ax.legend()
    save_fig(fig, outdir, "paper_policy_g_of_x_vs_k_by_alpha.png")


def summarize_best_configs(df: pd.DataFrame, outdir: Path) -> None:
    cols = [c for c in [
        "policy", "num_pages", "hot_pages", "batch_size", "k", "m", "alpha",
        "chosen_target_fraction", "final_fraction", "moved_pages", "g_of_x"
    ] if c in df.columns]

    ranked = df.sort_values("g_of_x", ascending=False)[cols].head(25)
    out_csv = outdir / "top_25_by_g_of_x.csv"
    ranked.to_csv(out_csv, index=False)
    print(f"wrote {out_csv}")


def apply_filters(df: pd.DataFrame, args: argparse.Namespace) -> pd.DataFrame:
    out = df.copy()

    if args.policy:
        out = out[out["policy"].isin(args.policy)]

    for col_name, value in [
        ("num_pages", args.num_pages),
        ("hot_pages", args.hot_pages),
        ("batch_size", args.batch_size),
        ("k", args.k),
        ("m", args.m),
        ("alpha", args.alpha),
    ]:
        if value is not None and col_name in out.columns:
            out = out[out[col_name] == value]

    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot CXL policy sweep CSV results.")
    parser.add_argument("csv", help="Path to input CSV")
    parser.add_argument("--outdir", default="plots", help="Output directory for plots")
    parser.add_argument("--policy", action="append", help="Filter to one or more policies")
    parser.add_argument("--num-pages", type=int, help="Filter to a single num_pages value")
    parser.add_argument("--hot-pages", type=int, help="Filter to a single hot_pages value")
    parser.add_argument("--batch-size", type=int, help="Filter to a single batch_size value")
    parser.add_argument("--k", type=float, help="Filter to a single k value")
    parser.add_argument("--m", type=float, help="Filter to a single m value")
    parser.add_argument("--alpha", type=float, help="Filter to a single alpha value")
    args = parser.parse_args()

    csv_path = Path(args.csv)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    df = pd.read_csv(csv_path)

    required = ["policy", "g_of_x"]
    require_columns(df, required)

    # Numeric cleanup for safety
    numeric_candidates = [
        "num_pages", "hot_pages", "rounds", "accesses_per_round", "batch_size",
        "tolerance", "r", "k", "m", "alpha", "chosen_target_fraction",
        "final_fraction", "local_pages", "remote_pages", "moved_pages",
        "promoted_pages", "demoted_pages", "rounds_with_migration",
        "cost1_in_cxl", "cost2_policy", "g_of_x",
    ]
    for col in numeric_candidates:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    df = apply_filters(df, args)

    if df.empty:
        raise ValueError("No rows left after filtering.")

    plot_avg_speedup_by_policy(df, outdir)

    for param in ["k", "m", "alpha"]:
        if param in df.columns and df[param].nunique() > 1:
            plot_param_lines(df, outdir, param)

    plot_fraction_tracking(df, outdir)
    plot_moved_pages_vs_speedup(df, outdir)
    plot_heatmap_for_paper_policy(df, outdir)
    plot_alpha_slices_for_paper_policy(df, outdir)
    summarize_best_configs(df, outdir)


if __name__ == "__main__":
    main()