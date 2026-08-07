#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "matplotlib"]
# ///
"""Compute LOD from blank-to-blank variation in exp1.csv (basic_LOD_swap run)."""

import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).parent
CSV = HERE / "exp1.csv"

# Only pull the numbered blanks (basic_LOD_swap output). Earlier rows in
# the file are from a prior firmware and are ignored.
final_re = re.compile(r"#\s*blank\s+(\d+)\s+final\s*=\s*([-\d.]+)\s*counts,\s*([-\d.]+)\s*V")
begin_re = re.compile(r"#\s*blank\s+(\d+)\s+begin\s+at\s+t=([-\d.]+)s")

finals = {}   # idx -> (counts, volts)
begins = {}   # idx -> t_start_s
for line in CSV.read_text().splitlines():
    if m := final_re.search(line):
        finals[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))
    elif m := begin_re.search(line):
        begins[int(m.group(1))] = float(m.group(2))

order = sorted(finals.keys())
idx = np.array(order)
counts = np.array([finals[i][0] for i in order])
volts = np.array([finals[i][1] for i in order])
t_min = np.array([begins.get(i, np.nan) for i in order]) / 60.0   # minutes from boot

n = len(counts)
mean_c = counts.mean()
std_c = counts.std(ddof=1)
cov = std_c / mean_c

# A_LOD = log10(mean / (mean - 3 sigma))
lod_a = np.log10(mean_c / (mean_c - 3 * std_c))
# Small-signal approx: A_LOD ~ (3 sigma / mean) / ln(10) = 1.303 * CoV
lod_a_approx = 3 * cov / np.log(10)

# Linear drift across the run (counts per minute), detrended std for comparison.
slope, intercept = np.polyfit(t_min, counts, 1)
resid = counts - (slope * t_min + intercept)
std_dt = resid.std(ddof=1)
cov_dt = std_dt / mean_c
lod_a_dt = np.log10(mean_c / (mean_c - 3 * std_dt))

print(f"n blanks           = {n}")
print(f"span               = {t_min[0]:.2f} -> {t_min[-1]:.2f} min "
      f"({(t_min[-1]-t_min[0]):.2f} min total)")
print(f"mean               = {mean_c:.4f} counts  ({volts.mean():.5f} V)")
print(f"std (sample, n-1)  = {std_c:.4f} counts")
print(f"CoV                = {cov*100:.4f} %")
print(f"3 sigma            = {3*std_c:.4f} counts")
print(f"A_LOD (exact)      = {lod_a:.5f}  (~ {lod_a*1000:.2f} mAU)")
print(f"A_LOD (1.303*CoV)  = {lod_a_approx:.5f}")
print()
print(f"linear drift       = {slope:.4f} counts/min  "
      f"({slope/mean_c*100:.4f} %/min)")
print(f"detrended std      = {std_dt:.4f} counts")
print(f"detrended CoV      = {cov_dt*100:.4f} %")
print(f"detrended A_LOD    = {lod_a_dt:.5f}  (~{lod_a_dt*1000:.2f} mAU)")
# Rough uncertainty on sigma at n=19: sigma_sigma / sigma ~ 1/sqrt(2(n-1))
sigma_rel_unc = 1 / np.sqrt(2 * (n - 1))
print(f"sigma rel. unc.    = {sigma_rel_unc*100:.1f} % at n={n}")

fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))
ax1, ax2, ax3 = axes

# (a) Series vs blank index with mean and ±3σ
ax1.plot(idx, counts, "o-", color="C0")
ax1.axhline(mean_c, color="k", lw=1, label=f"mean = {mean_c:.2f}")
ax1.axhline(mean_c + 3 * std_c, color="r", lw=1, ls="--", label="±3σ")
ax1.axhline(mean_c - 3 * std_c, color="r", lw=1, ls="--")
ax1.set_xlabel("blank #")
ax1.set_ylabel("ADC counts (mean of 10 avg rounds)")
ax1.set_title(f"blank series (n={n}, CoV={cov*100:.3f}%)")
ax1.legend()
ax1.grid(alpha=0.3)

# (b) Drift plot: counts vs time with linear fit
ax2.plot(t_min, counts, "o", color="C0", label="blanks")
tfit = np.array([t_min.min(), t_min.max()])
ax2.plot(tfit, slope * tfit + intercept, "k-", lw=1,
         label=f"fit: {slope:+.3f} counts/min\n({slope/mean_c*100:+.4f} %/min)")
ax2.set_xlabel("time since boot (min)")
ax2.set_ylabel("ADC counts")
ax2.set_title("drift check")
ax2.legend()
ax2.grid(alpha=0.3)

# (c) Histogram
ax3.hist(counts, bins=8, color="C0", edgecolor="k")
ax3.axvline(mean_c, color="k", lw=1)
ax3.axvline(mean_c - 3 * std_c, color="r", lw=1, ls="--")
ax3.axvline(mean_c + 3 * std_c, color="r", lw=1, ls="--")
ax3.set_xlabel("ADC counts")
ax3.set_ylabel("count")
ax3.set_title(f"A_LOD = {lod_a:.4f}  (~{lod_a*1000:.2f} mAU)")
ax3.grid(alpha=0.3)

fig.tight_layout()
out = HERE / "exp1_lod.png"
fig.savefig(out, dpi=140)
print(f"wrote {out}")
