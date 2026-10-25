# Stereo optical-flow ordering benchmark

Measured results and production implications are in
[optical-order-benchmark-results.md](optical-order-benchmark-results.md).

Run from the repository root in PowerShell:

```powershell
./scripts/benchmark-optical-order.ps1
```

The launcher builds a standalone C++ executable with Visual Studio C++ build
tools and a Windows SDK. The native helper is necessary to submit real D3D12
and NVIDIA Optical Flow work; this is not a CPU simulation. It uses the existing
production optical-flow sessions and shaders. No installed DLLs, game files,
runtime settings, or production source files are changed.

Defaults: two distinct 3000 x 3000 RGBA8 eyes, each with an A/B pair separated
by a synthetic 12-pixel translation; Medium preset, 4 x 4 vector grid, three
generated timestamps (1/4, 1/2, 3/4), eight warmups and 60 measured intervals
per case, repeated for three rounds. Subsequent rounds randomize schedule order
with a reproducible seed. Temporal hints are disabled to make output comparisons
deterministic; this does not measure the benefit of temporal hints in a game.

```powershell
# Two generated frames, equivalent to 3x total output:
./scripts/benchmark-optical-order.ps1 -GeneratedFrames 2

# Full-eye optical flow only:
./scripts/benchmark-optical-order.ps1 -Workloads Full -Samples 100 -Rounds 5

# Reduced optical-flow resolution; outputs keep their original dimensions:
./scripts/benchmark-optical-order.ps1 -FlowPercent 50 -Preset Fast

# 50% x 50% center, Medium 100% flow; periphery Fast 25% flow, all 4x4:
./scripts/benchmark-optical-order.ps1 -Workloads Foveated -CenterWidth 0.5 -CenterHeight 0.5 -Preset Medium -FlowPercent 100 -PeripheralPreset Fast -PeripheralFlowPercent 25 -Grid 4

# Focus on intermediate-frame parallelism and a bounded two-pair pipeline:
./scripts/benchmark-optical-order.ps1 -Workloads Foveated -Modes serial,consume-per-region,phase-queues,two-pair-pipeline

# Short correctness/state validation, not representative performance numbers:
./scripts/benchmark-optical-order.ps1 -Samples 2 -Rounds 1 -Warmup 1 -DebugLayer

# Repeat without rebuilding:
./scripts/benchmark-optical-order.ps1 -SkipBuild
```

Workloads:

| Workload | Optical jobs per stereo pair | Outputs |
|---|---:|---|
| Full | 2 | 3000 x 3000 per eye and generated timestamp |
| Center | 2 | 55% x 45% center; production flow halo included |
| Foveated | 4 | Same center plus a half-resolution whole-eye periphery |

`FlowPercent` scales the full-eye/center flow inputs; foveated periphery defaults
to half that percentage, overridden by `PeripheralFlowPercent`. `PeripheralPreset`
defaults to `Same` as the center; it can independently be Fast/Medium/Slow.
`CenterWidth` and `CenterHeight` control the normalized center rectangle.
`PeripheralOutputPercent` defaults to 50 and is separate from flow resolution.
All source eyes remain `Size` x `Size`. Center/periphery
results are separate surfaces: final foveated composition is excluded. `Center`
isolates the optical portion of a configuration with a non-optical periphery;
it does not benchmark motion-vector periphery warping.

Schedules:

| Mode | Ordering |
|---|---|
| serial | For each region: prepare, launch flow, immediately queue completion wait; interpolate after all regions |
| launch-all | Prepare and launch each region without completion waits; wait for all results before interpolation |
| prepare-all | Submit all luma preparations, launch all flow jobs, then wait and interpolate |
| consume-per-eye | Prepare/launch all jobs; wait for left flow and interpolate left, then wait for right flow and interpolate right |
| consume-per-region | Prepare/launch all jobs; wait only for the next region's flow and begin its first timestamp immediately; then finish that eye's remaining timestamps |
| first-stereo | Generate the first timestamp of BOTH eyes before their later timestamps, with region-specific waits |
| centers-first | Same first-stereo output policy, but launch both center flow jobs before either peripheral flow job |
| eye-queues | Separate NORMAL D3D12 queues for the two eyes, with explicit start and completion fences |
| phase-queues | Up to six NORMAL queues: independent interpolation of every eye/timestamp, sharing immutable sources and flow vectors |
| two-pair-pipeline | Same phase queues, with two GPU-resident adjacent pairs A/B and B/C using separate capture copies and optical sessions |

The original five modes interpolate all timestamps of an eye before the next
eye. `consume-per-region` additionally starts an eye's first timestamp before
its later timestamps; `phase-queues` and `two-pair-pipeline` remove the GPU order
dependency between timestamps. The benchmark does not reproduce the production
worker's entire frame-major command-list batching.

In the phase-queue modes the shared sources and vectors transition once to
read-only SRV state. Every output, command allocator and descriptor range is
independent. All readers join before shared resources return to COMMON or can
be reused. Merely sending the production `render()` calls to multiple queues
would race their shared-resource transitions; the benchmark avoids that.

Each case initializes its sessions before warmup. Every eye/region/timestamp
output is read back and hashed outside the timing interval. All schedules must
be bit-identical to the serial baseline and produce nonconstant image content;
otherwise the executable fails and the launcher does not publish a summary.
`-DebugLayer` also fails on D3D12 validation errors. Output equivalence verifies
scheduling correctness, not general interpolation image quality.

`batch_ms` is elapsed GPU-timestamp time from the common start marker until all
stereo outputs finish. `gpu_ms` divides that by `pairs` (1 or 2): compare it for
throughput, not for the latency of an individual pair. The multi-queue variants
join all their queues before the end
marker. It includes dependency waits and any gaps while the CPU submits work;
it is not a sum of busy-engine time. `cpu_ms` is CPU wall time through the single
batch completion wait, also divided by pair count. `first_output_ms` measures
time until the first interpolated timestamp of the first pair has completed
in both eyes (all optical regions); final composition and publication are absent.
Single-pair schedules have one pair in flight; the two-pair schedule has exactly
two. This first-result timestamp also exposes the delay from eye-major scheduling.

Results are saved under `build/optical-order-benchmark/runs/<timestamp>/`:
`metadata.json`, `build.log`, `run.log`, raw `samples.csv`, and `summary.csv/json`
with mean, median and p95 plus first-stereo-output mean and pair count. Compare median and p95 across rounds; small differences
within run-to-run variation do not establish a winner. Debug-layer timings are
diagnostic only. For more stable results, close the game and VR streamer first;
an additional run under load measures a different contention scenario.

This isolates scheduling inside one or two resident frame pairs. It excludes capture copies,
game rendering, frame pacing, final composition, headset upload, OpenXR, NVENC,
and real source arrival/render-ahead. Both A/B and B/C are already GPU-resident
at the start of a two-pair measurement; its first-result time does not include
waiting for C to be rendered. At 30 native FPS, waiting for that future input
could add about 33 ms and would defeat a latency optimization. A production
pipeline should start A/B immediately and overlap B/C only when C actually
arrives; it should never wait to fill a two-pair batch. Distinct sessions here
disable temporal hints; retaining useful temporal history in a production
pipeline requires additional ownership/synchronization design.

The benchmark therefore cannot directly explain a particular
Windows utilization percentage or predict headset latency. Separate sessions
allow overlapping requests but still share the optical-flow hardware capacity.
