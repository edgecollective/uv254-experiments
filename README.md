# uv254-experiments

note: experimenting with new button -- the GQ12H-10/S;  mounting hole size 12mm; action momentary

## TODO for main absorbance firmware (`src/main.cpp`)

- **Increase LED warm-up and discard first post-warm-up sample.** During LOD characterization (2026-06-29) the first ADC sample taken right after the existing 1.5 s warm-up was ~10 % low compared to subsequent samples; the LED clearly hadn't reached steady state. By the 2nd sample (1 s later) the reading was stable. Mitigation applied in `lod_test/` and worth porting back into the main firmware:
  - Bump `LED_WARMUP_MS` from 1500 to **3000**.
  - Take one analogRead after the warm-up timer fires and **discard it** before starting the averaging window.
  - This costs ~1.5 s extra per CALIBRATE/COMPUTE press, but removes a systematic first-sample bias that otherwise dominates the absorbance noise.
