#!/usr/bin/env python3
"""Compare repeated query times from jordan_method_benchmark for the named cases."""
import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import median

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


METHODS = [("Rank blocks", "rank_repeat_ms", "o", "#1768a0"),
           ("Streaming", "stream_repeat_ms", "s", "#b45b1b"),
           ("Automatic", "auto_repeat_ms", "^", "#087966")]


def read_times(path, cases):
    """Collect completed batch measurements in milliseconds per query."""
    times = defaultdict(list)
    with path.open(newline="") as stream:
        rows = csv.DictReader(stream)
        required = {"workload", "cache", "queries", *(field for _, field, _, _ in METHODS)}
        if not required.issubset(rows.fieldnames or ()):
            raise ValueError("Expected a measurement CSV from jordan_method_benchmark")
        for row in rows:
            case, policy = row["workload"], row["cache"]
            if case not in cases:
                continue
            queries = int(row["queries"])
            if queries <= 0 or policy not in {"reuse", "reset"}:
                raise ValueError(f"Invalid query count or cache policy for {case}")
            for _, field, _, _ in METHODS:
                milliseconds = float(row[field])
                if not math.isfinite(milliseconds) or milliseconds < 0:
                    raise ValueError(f"Invalid measurement for {case}: {field}")
                times[case, policy, field].append(milliseconds / queries)
    missing = set(cases) - {case for case, _, _ in times}
    if missing:
        raise ValueError("No completed measurements for " + ", ".join(sorted(missing)))
    return times


def draw(times, cases):
    policies = [p for p in ("reuse", "reset") if any(policy == p for _, policy, _ in times)]
    use_log = all(median(values) > 0 for values in times.values())
    fig, panels = plt.subplots(1, len(policies), sharex=True, sharey=True, squeeze=False,
                               figsize=(3 + 3.4 * len(policies), 2.3 + 0.6 * len(cases)))
    for ax, policy in zip(panels[0], policies):
        for (label, field, marker, color), offset in zip(METHODS, (-0.22, 0, 0.22)):
            x, y = [], []
            for index, case in enumerate(cases):
                values = times.get((case, policy, field))
                if values:
                    x.append(median(values))
                    y.append(index + offset)
            ax.scatter(x, y, label=label, marker=marker, color=color, s=34, zorder=3)
        ax.set_title("Cache " + policy)
        if use_log:
            ax.set_xscale("log")
        else:
            ax.set_xlim(left=0)
        ax.set_yticks(range(len(cases)), labels=cases)
        ax.set_ylim(len(cases) - 0.5, -0.5)
        ax.set_axisbelow(True)
        ax.grid(axis="x", color="#dedede")
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
    handles, labels = panels[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=3, frameon=False)
    fig.supxlabel("Median time per query in the repeated batch (ms)", fontsize=11)
    fig.tight_layout(rect=(0, 0.08, 1, 0.88))
    return fig


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("measurements", type=Path, help="Method benchmark measurement CSV")
    parser.add_argument("figure", type=Path, help="Output figure, for example methods.svg or methods.png")
    parser.add_argument("cases", nargs="+", help="Case names to plot, in display order")
    args = parser.parse_args()
    try:
        if args.figure.exists() and args.figure.samefile(args.measurements):
            raise ValueError("Choose a different file for the figure")
        cases = list(dict.fromkeys(args.cases))
        fig = draw(read_times(args.measurements, cases), cases)
        fig.savefig(args.figure, dpi=160)
    except (OSError, TypeError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
