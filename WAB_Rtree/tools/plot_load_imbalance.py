#!/usr/bin/env python3
import argparse
import csv
import math
import re
from pathlib import Path

import matplotlib.pyplot as plt

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
    "axes.titlesize": 14.0,
    "axes.labelsize": 12.5,
    "xtick.labelsize": 11.5,
    "ytick.labelsize": 11.5,
    "legend.fontsize": 11.5,
})


MARKER = "Per-DPU overlap totals (computed on host from returned sparse pairs):"
OVERLAP_RE = re.compile(r"DPU\[(\d+)\]\s+overlaps=(\d+)")
NR_DPUS_RE = re.compile(r"nr(\d+)", re.IGNORECASE)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Parse DPU overlap totals from logs and draw load-imbalance plots."
    )
    parser.add_argument(
        "logs",
        nargs="*",
        type=Path,
        help="Log files to parse. If omitted, uses the June 8 lake scaling logs.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("plots/load_imbalance_lake_10pct_20260608.png"),
        help="Output image path.",
    )
    parser.add_argument(
        "--stats-csv",
        type=Path,
        default=Path("plots/load_imbalance_lake_10pct_20260608_stats.csv"),
        help="Output CSV path for summary stats.",
    )
    parser.add_argument(
        "--keep-order",
        action="store_true",
        help="Plot raw DPU id order instead of sorting by descending overlap.",
    )
    parser.add_argument(
        "--ymax",
        type=float,
        default=None,
        help="Optional fixed y-axis upper bound shared across subplots.",
    )
    return parser.parse_args()


def default_logs():
    return [
        Path("logs/lake_10pct_scaling_20260608/lake_10pct_nr512.log"),
        Path("logs/lake_10pct_scaling_20260608/lake_10pct_nr1024.log"),
        Path("logs/lake_10pct_scaling_20260608/lake_10pct_nr2048.log"),
        Path("logs/lake_10pct_scaling_20260608/lake_10pct_nr2540.log"),
    ]


def parse_overlap_series(log_path: Path):
    lines = log_path.read_text(encoding="utf-8").splitlines()
    in_section = False
    overlaps = []

    for line in lines:
        if MARKER in line:
            in_section = True
            continue

        if not in_section:
            continue

        match = OVERLAP_RE.search(line)
        if match:
            overlaps.append((int(match.group(1)), int(match.group(2))))
            continue

        if overlaps:
            break

    if not overlaps:
        raise ValueError(f"No per-DPU overlap section found in {log_path}")

    overlaps.sort(key=lambda item: item[0])
    values = [value for _, value in overlaps]
    return values


def infer_nr_dpus(log_path: Path, values):
    match = NR_DPUS_RE.search(log_path.name)
    if match:
        return int(match.group(1))
    return len(values)


def percentile(sorted_values, q):
    if not sorted_values:
        return 0.0
    pos = (len(sorted_values) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return float(sorted_values[lo])
    frac = pos - lo
    return sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac


def summarize(values):
    n = len(values)
    mean = sum(values) / n
    variance = sum((x - mean) ** 2 for x in values) / n
    stdev = math.sqrt(variance)
    sorted_values = sorted(values)
    return {
        "count": n,
        "total": sum(values),
        "mean": mean,
        "stdev": stdev,
        "cv": (stdev / mean) if mean else 0.0,
        "min": min(values),
        "p50": percentile(sorted_values, 0.50),
        "p95": percentile(sorted_values, 0.95),
        "max": max(values),
        "max_over_mean": (max(values) / mean) if mean else 0.0,
    }


def plot_series(series_by_name, output_path: Path, keep_order: bool, ymax):
    nplots = len(series_by_name)
    ncols = 2 if nplots > 1 else 1
    nrows = math.ceil(nplots / ncols)
    shared_ymax = ymax
    if shared_ymax is None and nplots > 1:
        shared_ymax = max(max(values) for values in series_by_name.values()) * 1.05

    fig, axes = plt.subplots(nrows, ncols, figsize=(14, 3.8 * nrows), squeeze=False)
    axes_flat = axes.flatten()

    for ax in axes_flat[nplots:]:
        ax.axis("off")

    for ax, (label, values) in zip(axes_flat, series_by_name.items()):
        stats = summarize(values)
        plot_values = list(values) if keep_order else sorted(values, reverse=True)
        x = list(range(len(plot_values)))

        ax.plot(x, plot_values, color="#0f766e", linewidth=1.6, label="Per-DPU load")
        ax.fill_between(x, plot_values, color="#99f6e4", alpha=0.55)
        ax.axhline(stats["mean"], color="#b91c1c", linestyle="--", linewidth=1.3, label="Mean load")
        ax.set_title(
            f"{label} | CV={stats['cv']:.3f} | max/mean={stats['max_over_mean']:.2f}x",
            fontsize=14,
        )
        ax.set_xlabel("DPU rank" if not keep_order else "DPU id", fontsize=12.5)
        ax.set_ylabel("Overlap count", fontsize=12.5)
        ax.grid(True, axis="y", alpha=0.25)
        ax.tick_params(axis="both", labelsize=11.5)
        ax.legend(loc="upper right", frameon=True, framealpha=0.92, edgecolor="#cbd5e1")
        if shared_ymax is not None:
            ax.set_ylim(0, shared_ymax)

        note = (
            f"min={stats['min']:.0f}  mean={stats['mean']:.1f}  "
            f"p95={stats['p95']:.1f}  max={stats['max']:.0f}"
        )
        ax.text(
            0.01,
            0.97,
            note,
            transform=ax.transAxes,
            va="top",
            ha="left",
            fontsize=10.5,
            bbox={"boxstyle": "round,pad=0.25", "facecolor": "white", "alpha": 0.85, "edgecolor": "#cbd5e1"},
        )

    fig.suptitle("Load Imbalance Across DPUs", fontsize=17)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=220, bbox_inches="tight")
    plt.close(fig)


def make_label(log_path: Path, nr_dpus: int):
    stem = log_path.stem.lower()
    if "before" in stem:
        return f"Before | NR_DPUS={nr_dpus}"
    if "after" in stem:
        return f"After | NR_DPUS={nr_dpus}"
    if "10:55" in stem:
        return f"Before | NR_DPUS={nr_dpus}"
    if "10:57" in stem:
        return f"After | NR_DPUS={nr_dpus}"
    return f"NR_DPUS={nr_dpus} | {log_path.stem}"


def write_stats_csv(series_by_name, csv_path: Path):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "label",
                "count",
                "total",
                "mean",
                "stdev",
                "cv",
                "min",
                "p50",
                "p95",
                "max",
                "max_over_mean",
            ],
        )
        writer.writeheader()
        for label, values in series_by_name.items():
            row = {"label": label}
            row.update(summarize(values))
            writer.writerow(row)


def main():
    args = parse_args()
    log_paths = args.logs or default_logs()

    series_by_name = {}
    for log_path in log_paths:
        values = parse_overlap_series(log_path)
        nr_dpus = infer_nr_dpus(log_path, values)
        label = make_label(log_path, nr_dpus)
        if label in series_by_name:
            label = f"{label} #{len(series_by_name) + 1}"
        series_by_name[label] = values

    plot_series(series_by_name, args.output, args.keep_order, args.ymax)
    write_stats_csv(series_by_name, args.stats_csv)

    print(f"Saved plot: {args.output}")
    print(f"Saved stats: {args.stats_csv}")
    for label, values in series_by_name.items():
        stats = summarize(values)
        print(
            f"{label}: count={stats['count']} mean={stats['mean']:.2f} "
            f"cv={stats['cv']:.4f} max/mean={stats['max_over_mean']:.2f}x"
        )


if __name__ == "__main__":
    main()
