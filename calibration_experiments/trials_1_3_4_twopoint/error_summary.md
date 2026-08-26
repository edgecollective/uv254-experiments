# Trial 4 — two-point calibration comparison

Two-point cal approach                       | k       | A_anchor_raw | derivation
---                                          | ---     | ---          | ---
Cal #1: fluorescein anchor (firmware default)| 1.2092 | 0.0827       | k = 0.100 / A_fluor_raw
Cal #2: 0.115 water sample as anchor         | 1.3128 | 0.0876       | k = 0.115 / A_S3_raw

## Per-sample residuals (mAU = 0.001 cm⁻¹)

| RealTech (cm⁻¹) | DIY raw | corr(fluor) | corr(0.115) | err_raw (mAU) | err_fluor (mAU) | err_0.115 (mAU) | note |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- |
| 0.0130 | 0.0151 | 0.0182 | 0.0198 | +2.1 | +5.2 | +6.8 |  |
| 0.0170 | 0.0172 | 0.0208 | 0.0226 | +0.2 | +3.8 | +5.6 |  |
| 0.0280 | 0.0283 | 0.0342 | 0.0371 | +0.3 | +6.2 | +9.1 |  |
| 0.0380 | 0.0330 | 0.0398 | 0.0433 | -5.0 | +1.8 | +5.3 |  |
| 0.0400 | 0.0365 | 0.0442 | 0.0480 | -3.5 | +4.2 | +8.0 |  |
| 0.0910 | 0.0671 | 0.0811 | 0.0881 | -23.9 | -9.9 | -2.9 |  |
| 0.1010 | 0.0827 | 0.1000 | 0.1086 | -18.3 | -1.0 | +7.6 | (fluorescein anchor, cal #1) |
| 0.1150 | 0.0876 | 0.1059 | 0.1150 | -27.4 | -9.1 | +0.0 | (water anchor, cal #2) |

## Summary error statistics

All values in mAU (= 0.001 cm⁻¹). n is sample count in the subset.

### All 8 samples

| approach | n | bias | MAE | RMSE | max\|err\| |
| ---      | ---: | ---: | ---: | ---: | ---: |
| RAW (no correction) | 8 | -9.4 | 10.1 | 14.6 | 27.4 |
| Cal #1: fluorescein anchor (k=0.100/A_raw) | 8 | +0.2 | 5.1 | 5.9 | 9.9 |
| Cal #2: 0.115 water anchor (k=0.115/A_raw) | 8 | +4.9 | 5.6 | 6.3 | 9.1 |

### DOM only (fluorescein excluded, n=7)

| approach | n | bias | MAE | RMSE | max\|err\| |
| ---      | ---: | ---: | ---: | ---: | ---: |
| RAW (no correction) | 7 | -8.2 | 8.9 | 14.0 | 27.4 |
| Cal #1: fluorescein anchor (k=0.100/A_raw) | 7 | +0.3 | 5.7 | 6.3 | 9.9 |
| Cal #2: 0.115 water anchor (k=0.115/A_raw) | 7 | +4.5 | 5.4 | 6.1 | 9.1 |

### DOM excl. anchor 0.115 (fluor excluded and anchor excluded, n=6)

| approach | n | bias | MAE | RMSE | max\|err\| |
| ---      | ---: | ---: | ---: | ---: | ---: |
| RAW (no correction) | 6 | -5.0 | 5.8 | 10.1 | 23.9 |
| Cal #1: fluorescein anchor (k=0.100/A_raw) | 6 | +1.9 | 5.2 | 5.7 | 9.9 |
| Cal #2: 0.115 water anchor (k=0.115/A_raw) | 6 | +5.3 | 6.3 | 6.6 | 9.1 |

## How to read it

- **Bias** = mean signed error. Corrections should push this toward 0.
- **MAE** and **RMSE** = typical error magnitudes.
- **max|err|** = worst-case single-sample error.
- The anchor sample of each cal has residual = 0 by construction; excluding it (last table) gives a fair test of how well the cal *generalises*.
