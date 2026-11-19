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

## Experimental pipeline warmup

Add `--pipeline-warmup-limit 128` to a cache-enabled run to prebuild up to 128 saved Vulkan
pipelines in first-use order, with at most four workers. Use `0` to defer all Vulkan pipeline
creation to first use. Shader modules still load from the persistent cache. Compare warm startup
and P99 together, since deferred creation can move work from boot into gameplay.

## Experimental ZArchive streaming

Add `--archive-read-ahead` for a ZArchive workload with sequences of small reads. Once the
same thread reads adjacent data, it can serve subsequent reads from a 128 KiB window. Compare
`archive_backend_reads`, `archive_read_ahead_hits` and frame times against the same archive
without the switch. The window uses a weak reader reference so closing the archive still releases
the underlying file.

Run `archive_file_tests.exe --benchmark-sequential` from the Windows test artifact to capture
10 paired trials in CSV. The fixture reads an 8 MiB archive member in 4 KiB chunks and verifies
every byte. A local Windows Clang test on the Ryzen 7 5700X3D measured median times of 9.364 ms
with the switch off and 6.491 ms with it on. Backend calls decreased from 2,048 to 65 per trial.
This fixture measures sequential archive I/O. Game performance needs a separate capture.

## Automated validation

The Windows and Linux jobs build the emulator and regression targets. GitHub-hosted CI runs the
host tests. The Vulkan cases need a device with the full feature set used by the renderer test
harness. On a suitable Windows machine run `shader_recompiler_compute_tests.exe --shader-cache-only`
from the extracted Vulkan test artifact. With a local build, `ctest -L hardware_vulkan` runs the
entire renderer suite.

## Compare

Use the same benchmark instrumentation, scenario, game build, hardware, graphics driver, resolution and inputs for both revisions. Run five cold and five warm trials per revision. Record the save or replay and the start/stop conditions so someone else can repeat the scenario. Compare the medians of each per-run metric. Reject candidate changes with visual, stability or audio regressions. The default-enable gate is a 10% improvement in the target metric and at most 5% regression across the other reference scenarios.

The Python runner alternates emulator order and writes every raw capture, emulator log and a
median comparison to a new directory. Use a fixture that starts at the same point and runs the
same sequence without live input. If needed, supply a local save fixture with `--seed`.

```powershell
python tools/benchmark.py --baseline 'D:\Baseline\kyty_emulator.exe' --candidate 'D:\Candidate\kyty_emulator.exe' --game 'D:\Games\ExampleGame' --output '.\comparison' --candidate-arg=--pipeline-warmup-limit --candidate-arg=128
```

GTA VI status is **untested**. The compatibility milestones are first guest frame, menu, loaded save, interactive gameplay and a repeatable streaming-heavy driving route. Record game serial/version, emulator commit and the first concrete blocker when supported test files become available.
