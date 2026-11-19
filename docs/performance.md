# Performance benchmarks

KytyPs5 GTA VI is based on upstream commit `5ad27cdd1aeb48a7885b1a71d5a8321590b31ecd`.
The Windows target for the first measurement pass is a Ryzen 7 5700X3D and an RTX 3080.

## Capture

```powershell
kyty_emulator.exe --game 'D:\Games\ExampleGame' --shader-cache --benchmark-output cold.json --benchmark-cache-state cold --benchmark-seconds 120
```

Use a new output path inside an existing directory. The emulator writes the report after an orderly window exit. The time limit runs from benchmark initialization and is enforced by the window loop. Use an external process timeout to catch stalls before the window loop starts.

A benchmark begins before emulator initialization. It counts new, nonblank guest frames accepted by the host Vulkan swapchain. Frame intervals are measured at completion of the present call. Percentiles use nearest rank, and unavailable measurements are JSON null. The report includes the raw intervals, capped at 262,144 samples per run, and marks captures that exceeded the cap. On devices with memory-budget reporting, device memory is sampled at most twice per second while guest frames are presented. Peak resident memory is the operating system's process peak.

`--benchmark-cache-state` only labels the run. For a cold run use a fresh working directory. For a warm run reuse the corresponding cold run's working directory after an orderly exit and keep `--shader-cache` enabled for both. This isolates Kyty's local shader and pipeline data. Driver and OS caches may still persist across runs.

## Compare

Use the same benchmark instrumentation, scenario, game build, hardware, graphics driver, resolution and inputs for both revisions. Run five cold and five warm trials per revision. Record the save or replay and the start/stop conditions so someone else can repeat the scenario. Compare the medians of each per-run metric. Reject candidate changes with visual, stability or audio regressions. The default-enable gate is a 10% improvement in the target metric and at most 5% regression across the other reference scenarios.

GTA VI status is **untested**. The compatibility milestones are first guest frame, menu, loaded save, interactive gameplay and a repeatable streaming-heavy driving route. Record game serial/version, emulator commit and the first concrete blocker when supported test files become available.
