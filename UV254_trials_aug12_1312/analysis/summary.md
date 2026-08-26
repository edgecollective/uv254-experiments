# Aug 12 1312 — Ver 7.0 (2-pt affine) validation

Source: `data.xlsx`.

## Block A: Ver 7.0 on fluorescein solutions
(n = 8, RT range 0.008–0.101 cm⁻¹)

The firmware was calibrated with 0.010 and 0.100 fluorescein anchors. Back-solved
from the (raw, corr) pairs: **m = 1.2152, b = -1.26 mAU**
(essentially pure multiplicative — b ≈ 0).

| metric | raw | corrected |
|---|---|---|
| slope vs RT | 0.792 | 0.962 |
| intercept | +4.12 mAU | +3.74 mAU |
| R² | 0.9995 | 0.9994 |
| MAE vs RT | 6.83 mAU | **2.01 mAU** |
| max err | 17.00 mAU | 3.70 mAU |
| mean signed | -5.98 mAU | +1.91 mAU |
| all-positive resid? | False | **False** |

Compare to Aug 10 1533 fluor-only (Ver 6.1, 1-pt cal): MAE = 4.9 mAU, all-positive.
Here MAE = **2.0 mAU** and residuals swing both signs.

## Block B: Ver 6.1 vs Ver 7.0 head-to-head
(separate sample lists for each firmware; both against RealTech)

### Ver 6.1 (n = 11, 1-pt cal, k ≈ 1.113)

| metric | raw | corrected |
|---|---|---|
| slope vs RT | 0.877 | 0.998 |
| MAE vs RT | 4.15 mAU | **3.31 mAU** |
| max err | 25.00 mAU | 5.90 mAU |
| mean signed | +1.96 mAU | +3.31 mAU |
| all-positive resid? | — | True |

### Ver 7.0 (n = 13, 2-pt affine)

Firmware applied: **m = 1.2846, b = -6.87 mAU** (recovered
from the (raw, corr) pairs).

| metric | raw | corrected |
|---|---|---|
| slope vs RT | 0.789 | 1.013 |
| MAE vs RT | 3.24 mAU | **4.35 mAU** |
| max err | 6.30 mAU | 8.80 mAU |
| mean signed | +1.05 mAU | +3.75 mAU |
| all-positive resid? | — | False |

### Best possible affine (post-hoc least-squares) on same V7.0 raw values
m = 1.1941, b = -7.57 mAU → MAE = **2.55 mAU**.

The gap (firmware 4.3 vs best possible 2.6) is
the calibration-choice cost — how much the on-device 2-pt anchors differ from the
optimal fit over the whole sample range. The remainder is irreducible scatter.

## Plots
- `plot_v70_fluor_only.png` — Block A scatter + residuals
- `plot_v61_vs_v70_head_to_head.png` — Block B 4-panel comparison
- `plot_v70_firmware_vs_best_affine.png` — how close firmware got to the optimal 2-pt affine
