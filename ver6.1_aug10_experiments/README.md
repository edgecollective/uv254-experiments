# ver6.1 firmware — Aug 10 2026 experiment

Source: `UV254 trials_aug10_1030.xlsx`, rightmost block (columns R–V, rows 27–35).

## What Josh recorded

Josh used the ver6.1 firmware, which shows both `A_raw` and `A_corr` on the OLED after
the on-device 2-point calibration (blank + 0.100 cm⁻¹ fluorescein standard).
He recorded, for each sample:

- **RealTech** absorbance (reference)
- Detector **counts** (`trans.`) and **volts** (`V`) from the ver6.1 screen
- **raw abs** — `A_raw` = log10(I_blank / I_sample)
- **corr. abs** — `A_corr` = k · A_raw, where k = 0.100 / A_raw(fluorescein std)

Rows: 8 total (1 fluorescein anchor at RealTech = 0.101, plus 7 water samples).

## Headline results

- **Fluorescein anchor**: on-device `A_corr = 0.1003` vs true 0.1010 — the anchor pins
  itself back to the standard, as it should. Inferred `k = 1.204`.
- **Water samples (n = 7), corrected vs RealTech**
  - Mean bias: **−2.8 mAU** (DIY reads slightly low overall)
  - MAE: **4.7 mAU**, RMSE: **6.7 mAU**, worst point: **−14.7 mAU** (the RealTech = 0.092 sample)
  - Corrected fit vs RealTech: y = 0.785·x + 0.006, R² = 0.985

## Interpretation

This is what we predicted from the manual Trial-4 analysis:

- The multiplicative 2-pt calibration **perfectly anchors the fluorescein standard**
  (the last point on the corr fit line sits exactly on the reference).
- But the DIY still under-reads water samples by ~22 % (slope ≈ 0.78), and there is
  a small positive intercept (~+0.006 cm⁻¹).
- **Why**: fluorescein's absorption band is narrow and offset from 254 nm, while DOM
  absorbs broadly. The LED / detector bandwidth-averages the two differently, so the
  scale factor that pins fluorescein isn't quite the right scale factor for DOM. A
  purely multiplicative correction cannot remove that residual scale + offset error.
- No sign of the Trial-2 anomaly (~+12 mAU intercept). This session behaved like the
  "typical" sessions we've characterised.

## Comparison to Trial 4 (ver4.0 + manual 2-pt correction)

`plot_v61_vs_trial4_manual.png` overlays this Aug-10 corrected data on our earlier
Trial 4 hand-calibrated set. The two point clouds sit essentially on top of each other:

| dataset                         | slope | intercept | R²    |
|---------------------------------|------:|----------:|:------|
| ver6.1 Aug 10 (on-device corr)  | 0.885 | +0.0030   | 0.974 |
| Trial 4 (ver4.0 + manual corr)  | 0.815 | +0.0073   | 0.992 |

Firmware behaves as expected — the on-device correction matches what we would have
computed by hand, and the residual DOM slope/offset issue persists in both.

## Files

- `plot_v61_raw_and_corr_vs_realtech.png` — Josh-style: DIY raw and corrected vs
  RealTech, both fits, 1:1 line.
- `plot_v61_residuals.png` — same top panel plus DIY − RealTech residuals in mAU.
- `plot_v61_percent_error.png` — percent error per sample.
- `plot_v61_vs_trial4_manual.png` — ver6.1 corrected overlaid on Trial 4 hand-corrected.
- `error_summary.md` / `error_summary.csv` — fit and residual statistics, per-sample table.

## Next step to close the remaining ~0.02 cm⁻¹ residual error

Move from a single-anchor multiplicative correction to a 2-point **affine** correction
using two DOM standards (e.g. Suwannee River fulvic acid at two known concentrations)
instead of fluorescein. That would let `A_corr = m · A_raw + b` be fit against DOM's
actual spectral shape and remove both the residual slope (~0.78 → 1.0) and the small
positive intercept.
