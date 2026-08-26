# ver6.1 Aug 10 2026 experiment — error summary
Excel source: `UV254 trials_aug10_1030.xlsx`
Ver6.1 firmware — on-device 2-point calibration using the 0.100 cm⁻¹ fluorescein standard as the second anchor (blank + std).

**Rows analysed**: 8  
**Fluorescein-anchor row**: 1 (RealTech = 0.101, raw = 0.0833, corrected = 0.1003, k = 1.2041)

## Fit lines (DIY on y, RealTech on x)
|                     |  slope  | intercept | R²    |
|---------------------|--------:|----------:|:------|
| raw, all pts        | 0.7353 | +0.00253 | 0.9744 |
| corrected, all pts  | 0.8850 | +0.00303 | 0.9741 |
| raw, water only     | 0.6525 | +0.00502 | 0.9850 |
| corrected, water only | 0.7846 | +0.00605 | 0.9848 |

## Residual statistics (DIY − RealTech, mAU = milli-cm⁻¹)
| dataset | n | mean bias | MAE | RMSE | max |
|---|---:|---:|---:|---:|---:|
| raw, all pts | 8 | -10.38 | 10.40 | 13.78 | 27.70 |
| corrected, all pts | 8 | -2.58 | 4.20 | 6.31 | 14.70 |
| raw, water only (excl. fluor) | 7 | -9.33 | 9.36 | 13.12 | 27.70 |
| corrected, water only (excl. fluor) | 7 | -2.84 | 4.70 | 6.74 | 14.70 |

## Per-sample table
| # | RealTech | counts | V | raw abs | corr abs | raw−RT (mAU) | corr−RT (mAU) |
|--:|---------:|-------:|--:|--------:|---------:|-------------:|--------------:|
| **std** | 0.1010 | 850 | 0.623 | 0.0833 | 0.1003 | -17.70 | -0.70 |
| 1 | 0.0920 | 888 | 0.651 | 0.0643 | 0.0773 | -27.70 | -14.70 |
| 2 | 0.0280 | 981 | 0.719 | 0.0211 | 0.0254 | -6.90 | -2.60 |
| 3 | 0.0630 | 929 | 0.681 | 0.0448 | 0.0539 | -18.20 | -9.10 |
| 4 | 0.0400 | 952 | 0.698 | 0.0341 | 0.0411 | -5.90 | +1.10 |
| 5 | 0.0170 | 996 | 0.729 | 0.0147 | 0.0177 | -2.30 | +0.70 |
| 6 | 0.0370 | 956 | 0.7 | 0.0326 | 0.0392 | -4.40 | +2.20 |
| 7 | 0.0120 | 1002 | 0.734 | 0.0121 | 0.0145 | +0.10 | +2.50 |
