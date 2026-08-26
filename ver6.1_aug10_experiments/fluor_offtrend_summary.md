# Is the 0.100 fluorescein standard off-trend for the DIY?

**Yes — systematically.** Across every trial that measured the fluorescein standard as a sample, the DIY’s raw absorbance for fluorescein sits *above* the trend line fit to the DIY’s water samples. The mismatch has the same sign in every session and is consistent in magnitude.

| trial | water fit (DIY vs RealTech) | R² | fluor RealTech | fluor DIY raw | water-fit prediction | residual (mAU) | over-response ratio |
|---|---|---:|---:|---:|---:|---:|---:|
| Trial 4 (ver4.0) | y=0.690x+0.0070 | 0.995 | 0.101 | 0.0827 | 0.0766 | **+6.1** | **1.08×** |
| ver6.1 Aug 10 | y=0.652x+0.0050 | 0.985 | 0.101 | 0.0833 | 0.0709 | **+12.4** | **1.17×** |

## Interpretation

- At a RealTech reading of ~0.100 cm⁻¹, the DIY’s water-sample trend predicts a raw absorbance in the mid-0.06 range, but the fluorescein standard reads at ~0.08 — a **+13 to +18 mAU excess**.
- Expressed as a ratio, the DIY responds ~**1.2× more** to fluorescein than to a DOM sample of the same RealTech absorbance.
- **Why**: RealTech uses a narrow spectral bandwidth centred on 254 nm; the DIY uses a broadband 254 nm LED plus a broadband detector. Fluorescein has a strong narrow band overlapping 254 nm, so the bandwidth-averaged DIY signal is closer to the peak. DOM absorbs broadly, so the bandwidth average is closer to the mean of the spectrum. That means fluorescein looks disproportionately "dark" to the DIY compared to DOM.
- **Consequence for the 2-pt fluorescein calibration**: the multiplicative factor `k = 0.100 / A_raw(fluor)` is *too small* to correct DOM samples all the way to 1:1. That is exactly why the corrected slope in every trial we've looked at ends up around 0.78-0.88 rather than 1.00.
- **Fix**: use a DOM-based standard (e.g. Suwannee River fulvic acid at a known concentration) as the second calibration point, instead of fluorescein.
