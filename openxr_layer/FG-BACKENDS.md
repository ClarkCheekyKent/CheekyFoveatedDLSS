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

Frame records (`fg_buffered_frames.hpp`) are graphics-agnostic. Pacing reads
their identities, poses, timestamps and `FramePolicy`; color images are opaque
handles, and guide state is a backend payload (`FrameGuides12` or
`FrameGuides11` in `fg_frame_payloads.hpp`) that only its backend dereferences.

Graphics storage is grouped per API in `fg_buffered_resources.hpp`:
`BufferedResources12` (capture plans, direct eye captures, transports, FG
queues, cross-queue guide fence, direct-XR state) and `BufferedResources11`
(the immediate-context backend and composed quad eyes). The released-image map
`captured` is shared by both. Pacing never touches this storage directly; it
calls `FrameBackend::discard_captures` and `retire`. The storage is still
inherited by `BufferedPresenter` so the layer's capture hooks and the GPU
fixtures keep their names. Moving each block into its backend object is the
remaining ownership step; it needs accessors for the hooks in
`openxr_layer.cpp`/`quad_capture.inl` and about 100 fixture references, mostly
in `tests/openxr_binding/fg_async_validation.hpp`. The graphics entry-point
declarations are in `fg_buffered_entrypoints.inl`, and common
setup/diagnostic glue is in `fg_buffered_backend.inl`.

Regression checks are the default binding suite, `--fg-shader11`,
`--quad-d3d11`, `--quad-capture`, `--fg-hardening` and
`--fg-crop-gpu-validation`. The pre-existing `--fg-gpu-validation` failure at
"Live projection learns direct eye capture layout" remains separate from this
split.

Cross-API parity (`tests/openxr_binding/fg_parity_validation.hpp`, run by
`--fg-shader11`) feeds identical color, depth and motion to the native D3D11
backend and the D3D12 focus pipeline and compares every generated pixel. They
match except in one known band: the D3D12 center pass sees only the center
crop's guides, so within half the motion distance of the center edge a warp
whose source lies outside the crop falls back to the unwarped color. Native
D3D11 keeps full-eye guides there. Giving the D3D12 center pass full-eye
guides would remove the difference.
User-reported game checks after this split: DCS, Hogwarts Legacy and AC Rally
worked. ACC crash/hang investigation remains deferred.
