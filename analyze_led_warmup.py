"""Test the hypothesis: after 2-3 min of warm-up, LED output is fairly stable.

Splits led_drift_july7.csv at several warm-up cutoffs and reports drift
rate + noise for the post-cutoff data.  Writes led_drift_july7_warmup.png.
"""

# /// script
# dependencies = ["matplotlib", "numpy"]
# ///

import numpy as np
import matplotlib.pyplot as plt

CSV      = "led_drift_july7.csv"
OUT_PLOT = "led_drift_july7_warmup.png"

CUTOFFS_MIN = [0.0, 1.0, 2.0, 3.0, 5.0]

data = np.loadtxt(CSV, delimiter=",")
t_s   = data[:, 0]
adc   = data[:, 2]
elapsed_min = (t_s - t_s[0]) / 60.0

print(f"loaded {len(t_s)} rows, {elapsed_min[-1]:.2f} min total")
print(f"{'cutoff (min)':>12}  {'n':>4}  {'mean':>8}  "
      f"{'drift (cnt/min)':>16}  {'drift %/min':>11}  "
      f"{'σ_detrend':>10}  {'CoV_detrend':>11}  {'CoV_raw':>8}")

stats = []
for cutoff in CUTOFFS_MIN:
    mask = elapsed_min >= cutoff
    n = int(mask.sum())
    if n < 5:
        continue
    sub_t = elapsed_min[mask]
    sub_adc = adc[mask]
    mean_v = sub_adc.mean()
    slope, intercept = np.polyfit(sub_t, sub_adc, 1)
    fit = slope * sub_t + intercept
    residual = sub_adc - fit
    residual_std = residual.std(ddof=1)
    cov_detrend = 100.0 * residual_std / mean_v
    cov_raw     = 100.0 * sub_adc.std(ddof=1) / mean_v
    drift_pct   = 100.0 * slope / mean_v

    print(f"{cutoff:>12.1f}  {n:>4}  {mean_v:>8.3f}  "
          f"{slope:>+16.4f}  {drift_pct:>+11.4f}  "
          f"{residual_std:>10.4f}  {cov_detrend:>11.4f}  {cov_raw:>8.4f}")
    stats.append((cutoff, sub_t, sub_adc, slope, intercept, residual_std))

# --- Plot ---
fig, (ax_ts, ax_res) = plt.subplots(
    2, 1, figsize=(10, 7), sharex=False, gridspec_kw={"hspace": 0.3, "height_ratios": [3, 2]}
)

# Top: full time series, with linear fits from each cutoff.
ax_ts.plot(elapsed_min, adc, color="C0", linewidth=0.8, marker=".", markersize=3,
           label="5-s avg", zorder=2)
colors = ["0.7", "C1", "C2", "C3", "C4"]
for (cutoff, sub_t, sub_adc, slope, intercept, res_std), col in zip(stats, colors):
    fit = slope * sub_t + intercept
    ax_ts.plot(sub_t, fit, color=col, linewidth=1.4, linestyle="--",
               label=f"fit from t≥{cutoff:g} min: {slope:+.3f} cnt/min, "
                     f"±σ_detrend={res_std:.2f}", zorder=3)
    # shade the fit window's data range
    ax_ts.axvline(cutoff, color=col, linewidth=0.6, alpha=0.5, linestyle=":")

ax_ts.set_xlabel("elapsed time (min)")
ax_ts.set_ylabel("ADC count (5-s avg)")
ax_ts.set_title(f"led_drift_july7 — warm-up cutoff analysis "
                f"({len(t_s)} rows, {elapsed_min[-1]:.2f} min)")
ax_ts.legend(loc="lower right", framealpha=0.9, fontsize=8)
ax_ts.grid(True, alpha=0.3)

# Bottom: detrended residuals for the 2-min and 3-min cutoffs.
# (Best-case regions where the thesis is that we're on the stable plateau.)
for target_cutoff, col in [(2.0, "C2"), (3.0, "C3")]:
    match = [s for s in stats if s[0] == target_cutoff]
    if not match:
        continue
    cutoff, sub_t, sub_adc, slope, intercept, res_std = match[0]
    fit = slope * sub_t + intercept
    residual = sub_adc - fit
    ax_res.plot(sub_t, residual, color=col, linewidth=0.8, marker=".", markersize=3,
                label=f"t≥{cutoff:g} min: σ={res_std:.2f} cnt "
                      f"({100.0*res_std/sub_adc.mean():.3f}% CoV)")
    ax_res.axhline(0, color=col, linewidth=0.5, alpha=0.5)

ax_res.set_xlabel("elapsed time (min)")
ax_res.set_ylabel("residual from linear fit (counts)")
ax_res.set_title("Post-warm-up residuals (LED-only jitter after removing linear drift)")
ax_res.legend(loc="lower right", framealpha=0.9, fontsize=8)
ax_res.grid(True, alpha=0.3)

fig.savefig(OUT_PLOT, dpi=150, bbox_inches="tight")
print(f"\nwrote {OUT_PLOT}")

# --- Verdict ---
mean_full = adc.mean()
# Compare drift rate at cutoff=0 vs cutoff=3 min
d0 = [s for s in stats if s[0] == 0.0]
d3 = [s for s in stats if s[0] == 3.0]
if d0 and d3:
    _, _, _, slope0, _, _ = d0[0]
    _, _, _, slope3, _, res3 = d3[0]
    drift_pct_0 = 100.0 * slope0 / mean_full
    drift_pct_3 = 100.0 * slope3 / mean_full
    print(f"\ndrift rate all-data: {drift_pct_0:+.4f} %/min")
    print(f"drift rate t≥3 min: {drift_pct_3:+.4f} %/min "
          f"({drift_pct_3/drift_pct_0*100:.1f}% of all-data drift)")
    print(f"detrended σ (t≥3 min): {res3:.3f} counts "
          f"({100.0*res3/mean_full:.4f}% CoV)")
