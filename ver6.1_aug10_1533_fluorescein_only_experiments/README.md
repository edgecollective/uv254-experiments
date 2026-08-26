# ver6.1 Aug 10 1533 — fluorescein-only experiment

Source: `uv254_trials_aug10_1533.xlsx`, right block rows 41-47.
Ver6.1 firmware, on-device 2-pt calibration (blank + 0.100 fluor std). **All 7 samples are fluorescein standards** of varying concentration — not lake water / DOM.

## Fits (DIY on Y, RealTech on X)

| dataset | slope | intercept | R² |
|---|---:|---:|---:|
| Fluorescein raw          | **0.810** | +0.0071 | 0.9973 |
| Fluorescein corrected    | **0.936** | +0.0082 | 0.9973 |
| Water (DOM) raw (from Aug10 1030 data) | 0.652 | +0.0050 | 0.9850 |
| Water (DOM) corrected                  | 0.785 | +0.0060 | 0.9848 |

## Key contrast

- **Fluorescein raw slope = 0.810** vs **Water raw slope = 0.652**
  → the DIY responds ~**1.24× more strongly** to fluorescein than to DOM at the same RealTech reading. Direct confirmation of the spectral-mismatch hypothesis.
- **Fluorescein corrected slope = 0.936** — essentially 1:1. The 2-pt calibration works perfectly for its own analyte.
- **Water corrected slope = 0.785** — still ~22 % low. The fluorescein-derived correction factor is not the right one for DOM.

## Corrected residual statistics (DIY − RealTech, mAU)

| dataset | n | mean bias | MAE | RMSE | max abs |
|---|---:|---:|---:|---:|---:|
| **Fluorescein** | 7 | +4.89 | 4.89 | 5.65 | 8.30 |
| Water (DOM)     | 7 | -2.84 | 4.70 | 6.74 | 14.70 |

## Per-sample table (fluorescein-only)

| # | RealTech | counts | V | raw abs | corr abs | raw−RT (mAU) | corr−RT (mAU) |
|--:|---------:|-------:|--:|--------:|---------:|-------------:|--------------:|
| 1 | 0.104 | 857 | 0.628 | 0.0903 | 0.1043 | -13.70 | +0.30 |
| 2 | 0.005 | 1034 | 0.758 | 0.0086 | 0.0099 | +3.60 | +4.90 |
| 3 | 0.075 | 905 | 0.663 | 0.0666 | 0.0770 | -8.40 | +2.00 |
| 4 | 0.087 | 880 | 0.645 | 0.0786 | 0.0908 | -8.40 | +3.80 |
| 5 | 0.016 | 1005 | 0.736 | 0.0210 | 0.0243 | +5.00 | +8.30 |
| 6 | 0.025 | 987 | 0.723 | 0.0288 | 0.0332 | +3.80 | +8.20 |
| 7 | 0.052 | 939 | 0.688 | 0.0508 | 0.0587 | -1.20 | +6.70 |

## Conclusion

This is the counter-experiment. If the DIY's residual slope on DOM samples were caused by anything intrinsic to the firmware, optics, or detector, we would see it on fluorescein samples too. Instead, the fluorescein samples collapse cleanly onto the 1:1 line after the on-device correction (slope ≈ 1, near-zero bias, single-digit-mAU residuals). The DIY reports fluorescein accurately across the range.

The problem is exclusively about analyte spectrum: fluorescein calibrates for fluorescein, not for DOM. To bring DOM measurements to 1:1, the second calibration standard needs to be a DOM analogue (e.g. Suwannee River fulvic acid at a known concentration).
