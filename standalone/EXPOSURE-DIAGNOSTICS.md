# Exposure correction and support diagnostics

The DX12 Streamline path now preserves the game's exposure tag (13) when calling
private center and peripheral DLSS viewports. It explicitly clears the tag when
absent on the current frame, so an old exposure cannot leak into a later frame.

For an HDR output and a valid current 1x1 exposure texture, the SR red border and
calibration marker whites are normalized on the GPU:

    source_white = preExposure / (exposureTexture.red * exposureScale)

The finite positive result is bounded to [0.0001, 1024]. Invalid texture samples
use white=1. Missing/unsupported textures, unsupported resource states, SDR
outputs and auto-exposure-only paths retain the original debug brightness.
This is not a global 100x multiplier. ACC's DX11 auto-exposure path keeps its
original 1x debug rendering. No extra GPU-to-CPU readbacks or waits are added.
Marker normalization adds small GPU dispatches; it is not free GPU work.

Exposure is sampled on the same command list as debug rendering. The exposure
resource's tagged state is restored, and descriptor/resource lifetimes follow
the existing calibration fences and compositor recording lifetime tracking.
This corrects the supplied exposure scale, not arbitrary later local exposure,
color grading, or bloom. MSFS/ACC headset confirmation is still needed.

## Support capture

Use the existing support capture button. Exposure metadata logs run only during
that capture (up to five seconds), reset their quota on each request, and are
included in the normal log inside the ZIP. There is no separate switch and the
old .enable file is ignored. Outside capture there are no exposure log writes.
Records identify the original Streamline game's exposure texture separately from
private NGX evaluations. Failed Get results mean unavailable, not zero exposure.
Texture contents stay on the GPU and are not printed in the log.

Collect a ZIP from MSFS and ACC with the border enabled, in the cockpit and a
brighter view. Compare marker visibility, border color/glow and mapping lock.

## Validation

CheekyTests --debug-exposure exercises the production composite/marker shaders
on WARP with known exposure values: white=100, white=1, exposureScale=2,
zero/NaN texture samples and missing exposure. It checks red/neutral marker
pixels, black cells, alpha and unmodified scene pixels. Readbacks in this test
are test-only. The standalone Streamline host suite checks forwarding and
clearing exposure on both private viewports.
