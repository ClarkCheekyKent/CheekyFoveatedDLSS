# Optical-flow ordering: measured findings and production candidates

These are synthetic scheduling experiments, not a matched measurement of a
game's FG cost, a headset-latency measurement, or proof of a particular cause
of low reported GPU utilization. The useful evidence is the difference between
schedules running the same workload with identical output checks.

## Reproduction and retained evidence

Hardware: NVIDIA GeForce RTX 5070; driver 616.92. Measurements taken on
2026-10-24. Each schedule has 180 timed samples over three rounds, with eight
warmups before each case. Later rounds randomize schedule order.

Two distinct 3000 x 3000 RGBA8 eyes use synthetic translated A/B images.
The center is 0.5 x 0.5, with Medium optical flow at 100% resolution and a 4 x 4
grid. Periphery flow uses Fast at 25% and a 4 x 4 grid. **Periphery interpolation
output remains at 50%**, independently of flow resolution. Three interpolation
timestamps are generated for each eye, corresponding to 4x total output.

The production halo and alignment produce flow inputs of 1804 x 1804 for each
center and 752 x 752 for each periphery. Center and periphery interpolation
outputs are each 1500 x 1500. Temporal hints are disabled for deterministic
schedule comparisons. Both A/B and B/C are resident before a two-pair test starts.

The [parallel schedule run](optical-order-results/2026-10-24-rtx5070/half-center-parallel/)
and [first-stereo run](optical-order-results/2026-10-24-rtx5070/half-center-first-stereo/)
retain raw samples, summaries, metadata and native execution logs. Binaries and
intermediate build products are excluded. The recorded Git revision describes
the production sources before these new benchmark files were committed; the
helper evolved between runs. The first-stereo run also records its binary hash.

All tested region/eye/timestamp outputs matched their serial reference hashes.
The two-pair schedule also matched a separately generated B/C reference. This
checks output equivalence, not general interpolation image quality. The Windows
D3D12 debug layer was unavailable; no debug-layer validation is claimed.

```powershell
./scripts/benchmark-optical-order.ps1 -Size 3000 -CenterWidth 0.5 -CenterHeight 0.5 -Preset Medium -FlowPercent 100 -PeripheralFlowPercent 25 -PeripheralPreset Fast -PeripheralOutputPercent 50 -Grid 4 -Workloads Foveated -Samples 60 -Rounds 3
```

Methodology and other controls are documented in
[optical-order-benchmark.md](optical-order-benchmark.md).

## Measured comparisons

Times below are arithmetic means in milliseconds. The first column measures
completion of all optical interpolation outputs per native pair; for two pairs
it is total batch time divided by two. The second column is elapsed time until
the first timestamp of the first pair is ready for both eyes and all regions.
Final foveated composition and publication are excluded from both columns.

| Schedule | Processing per pair | First stereo interpolation ready | Run |
|---|---:|---:|---|
| Serial flow waits | 9.962 | 8.924 | parallel |
| Consume per eye | 7.479 | 6.429 | parallel |
| Consume per region | 7.295 | 6.226 | parallel |
| Separate eye queues | 7.552 | 7.549 | parallel |
| Separate interpolation phase queues | 7.474 | 7.471 | parallel |
| Two-pair pipeline | 6.865 | 11.206 | parallel |
| Consume per region, repeat | 7.189 | 6.149 | first-stereo |
| First stereo timestamp before later timestamps | 8.106 | 6.000 | first-stereo |
| Same, with both center flows launched first | 8.126 | 6.034 | first-stereo |

Deferring waits to region consumption reduced measured processing by about 27%
against this harness's serial baseline. This is not a predicted 27% game-FPS
improvement: the baseline's eye-major output scheduling and submission structure
do not reproduce the entire production worker.

Separate eye/phase queues did not beat the best single-queue region overlap in
these runs. Two resident pairs bought about 6% throughput versus region overlap,
while the first stereo result took about 80% longer. Prioritizing the first
stereo timestamp shaved only about 0.15 ms versus region overlap in the repeat
run, while adding about 0.92 ms to complete all interpolation phases. Center-first
flow submission did not demonstrate a useful advantage here.

These aggregate measurements do not reveal individual optical-engine execution
durations or prove how much actual overlap occurred. GPU elapsed time includes
dependency waits and submission gaps; per-stage instrumentation is still needed.

## Why these numbers do not explain the game screenshot

The screenshot showed 2888 x 2708 output, a 1588 x 1218 center and a 722 x 677
periphery. The benchmark's 1500 x 1500 periphery output has about 4.6 times as many
pixels. The HUD's reported periphery dimensions come from the periphery processing
scale; they are not the NVOFA estimation-grid dimensions. The screenshot alone
does not establish flow methods, temporal hints or the installed build.

The game's "Measured GPU stage sum" also combines capture per real frame,
generation evaluation timing, and upload per presentation. It is not an elapsed
native-pair generation measurement. Do not multiply that sum by the FG multiplier
or compare it directly with this benchmark's first-output marker.

At production revision 65b454b, `dlssg_worker.cpp` estimates optical flow only
for `frame_index == 1`. Subsequent batched phases restart the same timing-query
storage, and timing is collected only after the last phase. The HUD can therefore
describe the final phase rather than the complete batch. This source-level
diagnostics issue does not by itself quantify the screenshot/benchmark discrepancy.

Other differences include synthetic image content, disabled temporal hints, and
RGBA8 input. The harness excludes game rendering, capture, final composition,
publication copies, runtime work, encoding, and actual source arrival times.
The remaining 6 ms must not be described as unavoidable hardware latency.

## Applying the findings to production

1. **Make timing scopes comparable first.** Keep separate native-pair and
   per-phase query storage. Measure flow preparation, each flow dependency,
   interpolation, composition, publication, and first/last stereo readiness.
   Label per-real, per-generated and per-presentation counters explicitly.
   Keep GPU processing, queue delay and frame content age separate.

2. **Try region-level overlap within one pair.** Split `Session12::prepare`
   into preparation/flow submission and explicit completion consumption.
   Submit independent regions without immediately inserting a blocking GPU wait
   before preparing the next region. Wait for each result only before the shader
   that reads it. Reuse the same flow fields for every interpolation timestamp,
   as production already does. This is the strongest scheduling candidate from
   the controlled comparisons; validate with the actual game workload.

3. **Preserve resource ownership and temporal history.** A flow result must
   remain immutable until all phases reading it retire. Keep allocators,
   descriptors, input copies and generated outputs alive through their fences.
   Parallel readers need stable read-only resource states; moving the existing
   transition-heavy render calls onto different queues is insufficient. The
   two-pair test's independent sessions with hints disabled are not a drop-in
   production history solution.

4. **Consider publishing each completed stereo timestamp.** The production
   worker currently waits for the last generated phase before publishing the
   batch. Per-timestamp stereo completion fences could make the first result
   available earlier. Both eyes and final composition/publication must finish
   before exposing that timestamp. This possibility was not benchmarked end to
   end, and ordering by display deadline may matter more than finishing every
   phase of one eye first.

5. **Keep cross-pair overlap opportunistic and bounded.** Start A/B immediately;
   overlap B/C only when C actually arrives. Waiting to fill a two-pair batch at
   30 native FPS could add about 33 ms. Measure native hold time, oldest-frame
   age and first-output latency alongside throughput and VRAM. Higher utilization
   alone is not a success criterion.

6. **Retest under realistic contention before choosing queue topology.** Extra
   phase queues were not justified by the idle synthetic results. A dedicated FG
   queue versus the game's queue is a separate question: game work and NVENC
   were absent here. Match image formats, content, methods, all resolution knobs,
   temporal hints and multiplier; include composition/copies and real frame
   arrivals before selecting a production policy.

This branch preserves the experiment and proposed next steps. It makes no
production scheduling or diagnostics changes.
