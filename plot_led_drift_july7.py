"""Plot led_drift_july7.csv (columns: t_s, round, adc, volts)."""

# /// script
# dependencies = ["matplotlib", "numpy"]
# ///

import numpy as np
import matplotlib.pyplot as plt

CSV      = "led_drift_july7.csv"
OUT_TS   = "led_drift_july7.png"
OUT_HIST = "led_drift_july7_hist.png"

data = np.loadtxt(CSV, delimiter=",")
t_s   = data[:, 0]
adc   = data[:, 2]
v     = data[:, 3]

# Elapsed time from first row so the axis reads 0..N minutes.
elapsed_min = (t_s - t_s[0]) / 60.0

mean_adc = adc.mean()
std_adc  = adc.std(ddof=1)
cov_pct  = 100.0 * std_adc / mean_adc

# Linear fit for drift rate.
coeffs = np.polyfit(elapsed_min, adc, 1)
slope_per_min = coeffs[0]           # counts / minute
intercept     = coeffs[1]
fit           = np.polyval(coeffs, elapsed_min)
drift_pct_per_min = 100.0 * slope_per_min / mean_adc

fig, (ax_adc, ax_v) = plt.subplots(
    2, 1, figsize=(10, 6), sharex=True, gridspec_kw={"hspace": 0.08}
)

ax_adc.plot(elapsed_min, adc, color="C0", linewidth=0.9, marker=".", markersize=4,
            label="5-s avg")
ax_adc.plot(elapsed_min, fit, color="C3", linewidth=1.0, linestyle="--",
            label=(f"linear fit: {slope_per_min:+.3f} counts/min "
                   f"({drift_pct_per_min:+.3f} %/min)"))
ax_adc.axhline(mean_adc, color="0.5", linestyle=":", linewidth=0.7,
               label=f"mean = {mean_adc:.2f}  (±1σ {std_adc:.2f}, {cov_pct:.2f} % CoV)")
ax_adc.set_ylabel("ADC count (12-bit, 50-sample avg)")
ax_adc.legend(loc="lower right", framealpha=0.9, fontsize=8)
ax_adc.grid(True, alpha=0.3)

ax_v.plot(elapsed_min, v, color="C2", linewidth=0.9, marker=".", markersize=4)
ax_v.axhline(v.mean(), color="0.5", linestyle=":", linewidth=0.7,
             label=f"mean = {v.mean():.5f} V")
ax_v.set_xlabel("elapsed time (min)")
ax_v.set_ylabel("voltage (V)")
ax_v.legend(loc="lower right", framealpha=0.9, fontsize=8)
ax_v.grid(True, alpha=0.3)

fig.suptitle(f"led_drift_july7 (nRF52840, 555-driven LED, 50-sample avg @ 10 Hz -> row every 5 s, "
             f"n={len(t_s)} rows, {elapsed_min[-1]:.1f} min)")
fig.tight_layout()
fig.savefig(OUT_TS, dpi=150)

fig2, ax_h = plt.subplots(figsize=(8, 4))
adc_range = adc.max() - adc.min()
bins = max(20, int(adc_range * 2)) if adc_range > 0 else 20
ax_h.hist(adc, bins=bins, color="C0", edgecolor="white")
ax_h.set_xlabel("ADC count (5-s average)")
ax_h.set_ylabel("frequency")
ax_h.set_title(f"ADC 5-s average distribution (n={len(adc)}, "
               f"range {adc.min():.2f}..{adc.max():.2f})")
ax_h.grid(True, alpha=0.3, axis="y")
fig2.tight_layout()
fig2.savefig(OUT_HIST, dpi=150)

print(f"n rows = {len(t_s)}, duration = {elapsed_min[-1]:.2f} min")
print(f"ADC: mean = {mean_adc:.3f}, std = {std_adc:.3f}, CoV = {cov_pct:.3f} %, "
      f"range = {adc.min():.3f}..{adc.max():.3f}")
print(f"drift (linear fit): {slope_per_min:+.4f} counts/min "
      f"({drift_pct_per_min:+.4f} %/min of mean)")
print(f"span (last - first row): {adc[-1] - adc[0]:+.3f} counts "
      f"over {elapsed_min[-1]:.2f} min")
print(f"wrote {OUT_TS} and {OUT_HIST}")
