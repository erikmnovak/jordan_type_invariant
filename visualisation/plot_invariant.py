#!/usr/bin/env python3
"""Plot the text output of a Jordan type or filtered rank query."""
import argparse
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


def read_query(text):
    """Return (title, first index, values) for each series printed by the CLI."""
    series = []
    for line in text.splitlines():
        if not line.strip():
            continue
        label, separator, values = line.partition(":")
        tokens = values.split()
        if not separator or not tokens or not all(v.isdecimal() for v in tokens):
            raise ValueError("Expected a line of ranks or block counts from jordan_type")
        numbers = [int(v) for v in tokens]
        if label == "power ranks":
            series.append(("Power ranks", 0, numbers))
        elif label == "Jordan block counts":
            series.append(("Jordan block counts", 1, numbers))
        elif label == "filtered ranks":
            series.append(("Filtered ranks", 0, numbers))
        else:
            single = re.fullmatch(r"filtered rank\[(\d+)\]", label)
            if not single or len(numbers) != 1:
                raise ValueError(f"Unknown query output: {label}")
            series.append(("Filtered ranks", int(single[1]), numbers))
    labels = [title for title, _, _ in series]
    if labels == ["Power ranks", "Jordan block counts"] and len(series[0][2]) == len(series[1][2]) + 1:
        return series
    if labels == ["Filtered ranks"]:
        return series
    raise ValueError("Supply the complete output of one type or ranks query")


def draw(series):
    fig, panels = plt.subplots(1, len(series), squeeze=False, figsize=(3.8 * len(series), 3.2),
                               constrained_layout=True)
    for ax, (title, first, values) in zip(panels[0], series):
        # A single requested power may exceed the precision of a plot coordinate.
        # Place its marker at the center and label it with the exact CLI index.
        indices = range(len(values)) if len(values) == 1 else range(first, first + len(values))
        if title == "Jordan block counts":
            ax.bar(indices, values, width=0.65, color="#ad5518")
            ax.set(xlabel="Block size", ylabel="Number of blocks")
        else:
            ax.plot(indices, values, marker="o", color="#1768a0", clip_on=False, zorder=3)
            ax.set(xlabel="Power k", ylabel="Rank")
        ax.set_title(title)
        ax.set_xlim(indices[0] - 0.5, indices[-1] + 0.5)
        ax.set_ylim(0, max(1, max(values) * 1.12))
        if len(values) == 1:
            ax.set_xticks([0], labels=[str(first)])
        elif len(values) <= 10:
            ax.set_xticks(indices)
        else:
            ax.xaxis.set_major_locator(MaxNLocator(nbins=8, integer=True))
        if max(values) <= 8:
            ax.set_yticks(range(max(1, max(values)) + 1))
        else:
            ax.yaxis.set_major_locator(MaxNLocator(nbins=5, integer=True))
        ax.set_axisbelow(True)
        ax.grid(axis="y", color="#dedede")
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
    return fig


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="CLI output file, or - for standard input")
    parser.add_argument("figure", type=Path, help="Output figure, for example ranks.svg or ranks.png")
    args = parser.parse_args()
    try:
        if args.input != "-" and args.figure.exists() and args.figure.samefile(args.input):
            raise ValueError("Choose a different file for the figure")
        text = sys.stdin.read() if args.input == "-" else Path(args.input).read_text()
        fig = draw(read_query(text))
        fig.savefig(args.figure, dpi=160)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
