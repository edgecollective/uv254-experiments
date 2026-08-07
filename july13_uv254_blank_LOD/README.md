# july13 UV-254 blank LOD

## Experiment

Purpose: estimate the absorbance limit-of-detection (LOD) of the current
UV-254 rig from the run-to-run variation of *blank* (buffer-only)
measurements. If every blank read exactly the same value we could
resolve arbitrarily small absorbances; in practice the reading wanders
(LED drift, detector noise, cuvette placement) and the noise floor
becomes the LOD.

Hardware / firmware:

- Adafruit ItsyBitsy nRF52840, 12-bit SAADC, `AR_INTERNAL_3_0` (3.0 V FS)
- UV-254 LED driven by an external 555 oscillator
- Photodiode → transimpedance → rectifier → 88 Hz RC low-pass →
  ItsyBitsy A5 (DC envelope of the 555-modulated signal)
- Firmware: [`basic_LOD_swap/`](../basic_LOD_swap) — 2-min LED warm-up,
  then each blank measurement is 5 rounds of 5-second "settle" followed
  by 10 rounds of 5-second "average" (75 s per blank). Between blanks
  the operator pulls the cuvette out and reinserts it (swap), so the
  measured variation includes real cuvette re-seating and not just
  purely electronic noise.

Run: 19 blank cycles collected over ~40 min, saved as `exp1.csv`.
The value stored per blank is `# blank N final = ...`, the mean of the
10 avg rounds (500 raw samples).

## LOD calculation

Standard blank-noise LOD (3σ definition):

$$A_\text{LOD} \;=\; \log_{10}\!\left(\frac{\mu}{\mu - 3\sigma}\right)
\;\approx\; \frac{3\sigma}{\mu \ln 10} \;=\; 1.303\,\text{CoV}$$

where µ and σ are the mean and sample standard deviation (n−1) of the
19 blank ADC values. Intuition: the smallest absorbance we can call
"real" is the one that shifts the transmitted intensity by 3× the
blank's own scatter.

Results (n = 19 blanks, mean of 10 avg rounds each):

| quantity                        | value                       |
| ------------------------------- | --------------------------- |
| mean                            | 237.87 counts (0.1743 V)    |
| σ (sample, n−1)                 | 0.585 counts                |
| CoV                             | 0.246 %                     |
| 3σ                              | 1.76 counts                 |
| **A_LOD (exact)**               | **0.00322 (~3.2 mAU)**      |
| A_LOD (1.303·CoV approximation) | 0.00321                     |
| σ relative uncertainty at n=19  | ~17 %                       |

So the noise floor is about **3 mAU** — anything below that isn't
distinguishable from a blank swap. With n = 19 the σ estimate itself
is only good to ~17 %, so read the number as roughly 3 ± 0.5 mAU.

## Drift check

The 40-minute run also lets us look for slow drift underlying the
scatter:

- Linear fit: **−0.011 counts/min ≈ −0.0045 %/min** (a ~0.2 % drop
  over the full 40 min). Small and in line with prior LED-warmup data.
- Detrending the linear component barely changes the noise:
  CoV falls from 0.246 % to 0.241 % and A_LOD from 3.22 to 3.15 mAU.
  So the LOD here is dominated by short-timescale swap-to-swap
  variation (cuvette re-seating + fast noise), not by long-term drift.

## Files

- `exp1.csv` — raw per-round data, one row every 5 s during each
  blank, with `# blank N final = ...` summary comments for each blank
  and `# blank N begin at t=...` timestamps
- `analyze_lod.py` — parses the 19 numbered blanks, prints stats,
  writes the plot
- `exp1_lod.png` — three panels: blank series with ±3σ band,
  drift plot (counts vs time with linear fit), and histogram
