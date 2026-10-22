# Buffered frame-generation backends

`openxr_layer/fg_async.hpp` owns the three threads, native credits, frame
selection, history progression, runtime Wait/Begin/End ownership and timing.
Its capture assembly, GPU generation, readiness, publication and retirement
operations go through `FrameBackend` in `fg_frame_backend.hpp`. That interface
forward-declares the frame/image handles and contains no Direct3D types.

The implementations are:

- `fg_backend12_buffered.inl`: D3D12 transport, queue selection and dependencies,
  NGX or shader interpolation, batched/private swapchain uploads and the direct
  XR extension in `fg_direct_xr.inl`. D3D11 NGX sessions retain their existing
  D3D11-to-D3D12 bridge.
- `fg_backend11_buffered.inl`: native D3D11 shader capture and interpolation,
  private swapchain publication and runtime flushes. It uses `fg_backend11.hpp`
  for GPU work and does not create a D3D12 device or use NGX.

Backend selection happens before the threads start. Capture assembly runs under
`capture_mutex`; generation runs on the completion thread; publication runs on
the presentation thread. Readiness only polls. The D3D11 backend retains its
single-immediate-context ordering, dispatch gate, pipeline-state restoration
and post-runtime flushes. The D3D12 backend retains its fence retirement and
submitted/prepared callbacks. Neither implementation changes pacing policy.

This first split preserves the existing layer capture hooks and graphics test
fixtures: frame records live in `fg_buffered_frames.hpp`, and graphics storage
is grouped in `BufferedGpuResources`. That storage is still host-owned and
public to those hooks/fixtures; complete resource encapsulation is a later
step. The graphics entry-point declarations are in
`fg_buffered_entrypoints.inl`, and common setup/diagnostic glue is in
`fg_buffered_backend.inl`.

Regression checks are the default binding suite, `--fg-shader11`,
`--quad-d3d11`, `--quad-capture`, `--fg-hardening` and
`--fg-crop-gpu-validation`. The pre-existing `--fg-gpu-validation` failure at
"Live projection learns direct eye capture layout" remains separate from this
split. Cross-API pixel parity coverage is the next planned test addition.
User-reported game checks after this split: DCS, Hogwarts Legacy and AC Rally
worked. ACC crash/hang investigation remains deferred.
