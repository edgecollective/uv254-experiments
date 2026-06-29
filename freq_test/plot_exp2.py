#!/usr/bin/env -S uv run --with matplotlib --with numpy --script
import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

here = Path(__file__).parent
csv_path = here / "exp2.csv"
out_path = here / "exp2.png"

freqs, counts = [], []
with csv_path.open() as f:
    reader = csv.DictReader(f)
    for row in reader:
        freqs.append(float(row["freq"]))
        counts.append(float(row["count"]))

fig, ax = plt.subplots(figsize=(8, 5))
ax.plot(freqs, counts, marker="o", linewidth=1.2, markersize=4)
ax.set_xlabel("LED modulation frequency (Hz)")
ax.set_ylabel("ADC count (avg)")
ax.set_title("ADC count vs LED modulation frequency (tap water)")
ax.xaxis.set_major_locator(mticker.MultipleLocator(100))
ax.grid(True, which="both", alpha=0.3)
fig.tight_layout()
fig.savefig(out_path, dpi=150)
print(f"Wrote {out_path}")
