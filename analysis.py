import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt

BURGUNDY, AMARANTH, CREAM, VANILLA = "#800020", "#A42E4A", "#FFFDD0", "#F7EDB9"

# (csv column, title, unit). The first panel is the one the comparison is about.
PANELS = [
    ("drop_pct", "Drop rate", "%"),
    ("processed", "Packets processed", "packets"),
    ("dropped", "Packets dropped", "packets"),
    ("avg_wait_ms", "Average waiting time", "ms"),
    ("buffer_util_pct", "Buffer utilisation", "%"),
    ("throughput_pps", "Throughput", "packets/s"),
]


def read_csv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return {key: [float(r[key]) for r in rows] for key in rows[0]}


def fmt(v):
    return f"{v:,.0f}" if abs(v) >= 100 or v == int(v) else f"{v:.1f}"


def make_figure(data, x_col, x_label, title, subtitle, out_path, log_x):
    x = data[x_col]
    fig, axes = plt.subplots(2, 3, figsize=(15, 8.6), facecolor=CREAM)
    fig.suptitle(f"{title}\n{subtitle}", fontsize=15, fontweight="bold", color=BURGUNDY)

    for i, (ax, (col, name, unit)) in enumerate(zip(axes.flat, PANELS)):
        y = data[col]
        main = i == 0
        ax.set_facecolor(VANILLA if main else CREAM)
        ax.plot(x, y, marker="o", color=BURGUNDY if main else AMARANTH,
                linewidth=3 if main else 2, markersize=7 if main else 5)
        for xi, yi in zip(x, y):
            ax.annotate(fmt(yi), (xi, yi), textcoords="offset points", xytext=(0, 9),
                        ha="center", fontsize=8, color="#3b0a15")
        if log_x:
            ax.set_xscale("log")
            ax.minorticks_off()
        ax.set_xticks(x)
        ax.set_xticklabels([f"{v:g}" for v in x], fontsize=8)
        ax.set_title(f"{name} ({unit})", fontsize=12, fontweight="bold" if main else "normal",
                     color=BURGUNDY)
        ax.set_xlabel(x_label, fontsize=9)
        ax.margins(y=0.18)
        ax.grid(alpha=0.3)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)

    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out_path, dpi=150, facecolor=CREAM)
    plt.close(fig)
    print("wrote", out_path)


def main():
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")

    d = read_csv(folder / "buffer_vs_drop.csv")
    make_figure(
        d, "buffer", "Buffer capacity (packets)",
        "Buffer capacity vs drop rate",
        f"arrival rate fixed at {d['lambda_pps'][0]:g} packets/s, {d['packets'][0]:,.0f} packets per run",
        folder / "buffer_capacity_graphs.png", log_x=True)

    d = read_csv(folder / "arrival_vs_drop.csv")
    make_figure(
        d, "lambda_pps", "Arrival rate (packets/s)",
        "Arrival rate vs drop rate",
        f"buffer fixed at {d['buffer'][0]:g} packets, {d['packets'][0]:,.0f} packets per run",
        folder / "arrival_rate_graphs.png", log_x=False)


if __name__ == "__main__":
    main()