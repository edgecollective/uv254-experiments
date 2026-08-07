#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy", "matplotlib"]
# ///
"""
Analyze exp1.csv (basic_LOD_swap, 20 blanks) for:
  (a) overall stability + LOD
  (b) whether the 5x5s settle and 10x5s average are appropriately sized
"""

import re
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).parent
CSV = HERE / "exp1.csv"

# ---------------------- parse ----------------------
# Data rows: t_s, phase, subphase, blank_idx, round, adc, volts
# Comments: "# blank N final = X counts, Y V"
#           "# blank N begin at t=T s"
data_re  = re.compile(r"^([\d.]+),(\w+),(\w+),(\d+),(\d+),([\d.]+),([\d.]+)\s*$")
final_re = re.compile(r"#\s*blank\s+(\d+)\s+final\s*=\s*([-\d.]+)\s*counts,\s*([-\d.]+)\s*V")
begin_re = re.compile(r"#\s*blank\s+(\d+)\s+begin\s+at\s+t=([-\d.]+)s")

settle_rounds = {}   # idx -> list[(round, counts)]
avg_rounds    = {}   # idx -> list[(round, counts)]
finals        = {}
begins        = {}

for line in CSV.read_text().splitlines():
    if m := data_re.match(line):
        t, phase, sub, bidx, rnd, adc, _v = m.groups()
        bidx, rnd, adc = int(bidx), int(rnd), float(adc)
        if phase == "blank" and sub == "settle":
            settle_rounds.setdefault(bidx, []).append((rnd, adc))
        elif phase == "blank" and sub == "avg":
            avg_rounds.setdefault(bidx, []).append((rnd, adc))
    elif m := final_re.search(line):
        finals[int(m.group(1))] = (float(m.group(2)), float(m.group(3)))
    elif m := begin_re.search(line):
        begins[int(m.group(1))] = float(m.group(2))

order  = sorted(finals.keys())
idx    = np.array(order)
counts = np.array([finals[i][0] for i in order])
t_min  = np.array([begins.get(i, np.nan) for i in order]) / 60.0

# ---------------------- (a) blank-to-blank LOD ----------------------
n      = len(counts)
mean_c = counts.mean()
std_c  = counts.std(ddof=1)
cov    = std_c / mean_c
lod_a  = np.log10(mean_c / (mean_c - 3 * std_c))
slope, intercept = np.polyfit(t_min, counts, 1)
resid  = counts - (slope * t_min + intercept)
std_dt = resid.std(ddof=1)
cov_dt = std_dt / mean_c
lod_a_dt = np.log10(mean_c / (mean_c - 3 * std_dt))

print("=" * 60)
print(f"BLANK-TO-BLANK  (n={n})")
print(f"  mean       = {mean_c:.3f} counts")
print(f"  std        = {std_c:.3f} counts")
print(f"  CoV        = {cov*100:.4f} %")
print(f"  3 sigma    = {3*std_c:.3f} counts")
print(f"  A_LOD      = {lod_a:.5f}  (~{lod_a*1000:.2f} mAU)")
print(f"  span       = {t_min[0]:.2f} -> {t_min[-1]:.2f} min "
      f"({t_min[-1]-t_min[0]:.2f} min)")
print(f"  drift      = {slope:.4f} counts/min  ({slope/mean_c*100:.4f} %/min)")
print(f"  detr. std  = {std_dt:.3f} counts   CoV = {cov_dt*100:.4f} %")
print(f"  detr. A_LOD= {lod_a_dt:.5f}  (~{lod_a_dt*1000:.2f} mAU)")

# ---------------------- (b) within-blank / settle diagnostics ----------------------
# For each blank, build the 5 settle values + 10 avg values (all in counts,
# each a 50-sample mean = one 5s round).
per_blank_all_15 = []   # shape (n_blanks, 15)
for i in order:
    s = [c for _, c in sorted(settle_rounds.get(i, []))]
    a = [c for _, c in sorted(avg_rounds.get(i, []))]
    if len(s) == 5 and len(a) == 10:
        per_blank_all_15.append(s + a)
per_blank_all_15 = np.array(per_blank_all_15)
n_full = per_blank_all_15.shape[0]

# Mean trace across blanks: is there still drift left when we START averaging?
mean_trace = per_blank_all_15.mean(axis=0)
sem_trace  = per_blank_all_15.std(axis=0, ddof=1) / np.sqrt(n_full)

# Test whether the settle window is long enough:
# Compare avg-rounds 1..3 to avg-rounds 8..10 across all blanks. If they're
# statistically the same, the settle was long enough; if avg_1..3 is
# systematically higher/lower than avg_8..10, we're still equilibrating.
avg_block   = per_blank_all_15[:, 5:15]
early_mean  = avg_block[:, :3].mean(axis=1)   # avg rounds 1..3
late_mean   = avg_block[:, -3:].mean(axis=1)  # avg rounds 8..10
diff        = late_mean - early_mean
diff_mean   = diff.mean()
diff_sd     = diff.std(ddof=1)
diff_sem    = diff_sd / np.sqrt(n_full)
# rough t: is (late-early) different from 0?
t_stat = diff_mean / diff_sem if diff_sem > 0 else float("nan")

# Test whether averaging longer would help:
# std of 10 avg rounds within a blank, per blank, root-N reduced.
within_std_per_blank = avg_block.std(axis=1, ddof=1)
within_std_mean      = within_std_per_blank.mean()
sem_of_avg           = within_std_mean / np.sqrt(10)  # SEM of the 10-round mean
# Blank-to-blank scatter of finals (already have std_c) is the swap-limited noise.

print()
print(f"WITHIN-BLANK  (n_full_blanks={n_full})")
print(f"  mean within-blank std over 10 avg rounds = {within_std_mean:.3f} counts")
print(f"  => SEM of a 10-round mean               = {sem_of_avg:.3f} counts")
print(f"  blank-to-blank std (already got)        = {std_c:.3f} counts")
print(f"  ratio (blank-to-blank) / (SEM of avg)   = {std_c/sem_of_avg:.2f}")
print()
print(f"SETTLE CHECK  (avg rounds 1..3 vs 8..10)")
print(f"  late - early = {diff_mean:+.3f} counts (SEM {diff_sem:.3f})  t~{t_stat:+.2f}")
print(f"  => |t| < ~2 means no residual settling detectable")

# ---------------------- plots ----------------------
fig, axes = plt.subplots(2, 2, figsize=(13, 9))
(ax_series, ax_drift), (ax_within, ax_hist) = axes

# 1. Blank series with ±3σ
ax_series.plot(idx, counts, "o-", color="C0")
ax_series.axhline(mean_c, color="k", lw=1, label=f"mean = {mean_c:.2f}")
ax_series.axhline(mean_c + 3 * std_c, color="r", lw=1, ls="--", label="±3σ")
ax_series.axhline(mean_c - 3 * std_c, color="r", lw=1, ls="--")
ax_series.set_xlabel("blank #")
ax_series.set_ylabel("ADC counts (mean of 10 avg rounds)")
ax_series.set_title(f"blank series (n={n}, CoV={cov*100:.3f}%)")
ax_series.set_xticks(np.arange(2, idx.max() + 1, 2))
ax_series.legend()
ax_series.grid(alpha=0.3)

# 2. Drift vs time with linear fit
ax_drift.plot(t_min, counts, "o", color="C0")
tfit = np.array([t_min.min(), t_min.max()])
ax_drift.plot(tfit, slope * tfit + intercept, "k-", lw=1,
              label=f"{slope:+.3f} counts/min ({slope/mean_c*100:+.4f} %/min)")
ax_drift.set_xlabel("time since boot (min)")
ax_drift.set_ylabel("ADC counts")
ax_drift.set_title(f"drift check (detr. A_LOD = {lod_a_dt:.4f})")
ax_drift.legend()
ax_drift.grid(alpha=0.3)

# 3. Within-blank profile: mean of settle+avg rounds, shaded ±SEM,
#    with vertical line marking where averaging starts.
xs = np.arange(1, 16)              # 1..5 = settle, 6..15 = avg 1..10
ax_within.plot(xs[:5], mean_trace[:5], "o-", color="0.4", label="settle")
ax_within.plot(xs[5:], mean_trace[5:], "o-", color="C0", label="avg")
ax_within.fill_between(xs, mean_trace - sem_trace, mean_trace + sem_trace,
                       color="C0", alpha=0.2)
ax_within.axvline(5.5, color="k", lw=1, ls="--", alpha=0.5,
                  label="settle → avg boundary")
ax_within.set_xlabel("round # inside a blank  (1..5 settle, 6..15 = avg 1..10)")
ax_within.set_ylabel("mean counts across all blanks")
ax_within.set_title("within-blank profile (mean ± SEM)")
ax_within.legend()
ax_within.grid(alpha=0.3)

# 4. Histogram of finals
ax_hist.hist(counts, bins=8, color="C0", edgecolor="k")
ax_hist.axvline(mean_c, color="k", lw=1)
ax_hist.axvline(mean_c - 3 * std_c, color="r", lw=1, ls="--")
ax_hist.axvline(mean_c + 3 * std_c, color="r", lw=1, ls="--")
ax_hist.set_xlabel("ADC counts")
ax_hist.set_ylabel("count")
ax_hist.set_title(f"A_LOD = {lod_a:.4f}  (~{lod_a*1000:.2f} mAU)")
ax_hist.grid(alpha=0.3)

fig.tight_layout()
out = HERE / "exp1_analysis.png"
fig.savefig(out, dpi=140)
print(f"\nwrote {out}")
