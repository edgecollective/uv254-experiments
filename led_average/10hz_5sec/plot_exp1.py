"""Plot led_average/10hz_5sec/exp1.csv (columns: t_s, adc, volts, led)."""

# /// script
# dependencies = ["matplotlib", "numpy"]
# ///

import numpy as np
import matplotlib.pyplot as plt

CSV      = "exp1.csv"
OUT_TS   = "exp1.png"
OUT_HIST = "exp1_hist.png"
SUPTITLE = ("led_average 10Hz/5s exp1 (ItsyBitsy M0, TCC0 NPWM 200 Hz, "
            "12-bit ADC, 50-sample avg @ 10 Hz -> row every 5 s)")

# Y-axis maxes (chosen to frame the signal without going all the way to full-scale).
ADC_YMAX = 800
V_YMAX   = 0.62

data = np.loadtxt(CSV, delimiter=",")
t   = data[:, 0]
adc = data[:, 1]
v   = data[:, 2]
led = data[:, 3].astype(int)

on = led == 1
adc_on = adc[on]
v_on   = v[on]
mean_adc_on = adc_on.mean()
std_adc_on  = adc_on.std(ddof=1)
cov_on_pct  = 100.0 * std_adc_on / mean_adc_on

fig, (ax_adc, ax_v) = plt.subplots(
    2, 1, figsize=(10, 6), sharex=True, gridspec_kw={"hspace": 0.08}
)

ax_adc.plot(t, adc, color="C0", linewidth=0.9, marker=".", markersize=4)
ax_adc.axhline(mean_adc_on, color="C3", linestyle="--", linewidth=0.8,
               label=f"LED-ON mean = {mean_adc_on:.2f}")
ax_adc.fill_between(
    t, mean_adc_on - std_adc_on, mean_adc_on + std_adc_on,
    color="C3", alpha=0.15,
    label=f"±1σ ({std_adc_on:.2f} counts, {cov_on_pct:.2f}% CoV)"
)
ax_adc.set_ylim(0, ADC_YMAX)
ax_adc.set_ylabel("ADC count (12-bit, 50-sample avg)")
ax_adc.legend(loc="upper right", framealpha=0.9)
ax_adc.grid(True, alpha=0.3)

ax_v.plot(t, v, color="C2", linewidth=0.9, marker=".", markersize=4)
ax_v.axhline(v_on.mean(), color="C3", linestyle="--", linewidth=0.8,
             label=f"LED-ON mean = {v_on.mean():.5f} V")
ax_v.set_ylim(0, V_YMAX)
ax_v.set_xlabel("time (s)")
ax_v.set_ylabel("voltage (V)")
ax_v.legend(loc="upper right", framealpha=0.9)
ax_v.grid(True, alpha=0.3)

off = led == 0
if off.any():
    edges = np.diff(off.astype(int))
    starts = np.where(edges == 1)[0] + 1
    ends   = np.where(edges == -1)[0] + 1
    if off[0]:  starts = np.r_[0, starts]
    if off[-1]: ends   = np.r_[ends, len(t)]
    for s, e in zip(starts, ends):
        for ax in (ax_adc, ax_v):
            ax.axvspan(t[s], t[e - 1], color="gray", alpha=0.18)

fig.suptitle(SUPTITLE)
fig.tight_layout()
fig.savefig(OUT_TS, dpi=150)

fig2, ax_h = plt.subplots(figsize=(8, 4))
adc_range = adc_on.max() - adc_on.min()
bins = max(20, int(adc_range * 4)) if adc_range > 0 else 20
ax_h.hist(adc_on, bins=bins, color="C0", edgecolor="white")
ax_h.set_xlabel("ADC count (LED ON only)")
ax_h.set_ylabel("frequency")
ax_h.set_title(f"LED-ON ADC distribution (n = {on.sum()}, "
               f"range {adc_on.min():.2f}..{adc_on.max():.2f})")
ax_h.grid(True, alpha=0.3, axis="y")
fig2.tight_layout()
fig2.savefig(OUT_HIST, dpi=150)

print(f"n total = {len(t)}, LED-on = {on.sum()}, LED-off = {(~on).sum()}")
print(f"LED-ON ADC: mean = {mean_adc_on:.3f}, std = {std_adc_on:.3f}, "
      f"CoV = {cov_on_pct:.3f}%, range = {adc_on.min():.3f}..{adc_on.max():.3f}")
print(f"wrote {OUT_TS} and {OUT_HIST}")
