#pragma once

namespace cheeky::foveated_dlss {

inline constexpr char d3d11_composite_shader_source[] = R"(
Texture2D<float4> LowResolutionColor : register(t0);
Texture2D<float4> DlssColor : register(t1);
RWTexture2D<float4> GameOutput : register(u0);

cbuffer Constants : register(b0) {
    uint2 OutputSize;
    uint2 OutputOrigin;
    uint2 InputBase;
    uint2 InputSize;
    uint2 RectBase;
    uint2 RectSize;
    float ShapeWidth;
    float ShapeHeight;
    float ShapeOffsetX;
    float ShapeOffsetY;
    float ShapeRoundness;
    float Feather;
    uint2 DlssOrigin;
    uint ShowAlignmentBorder;
    float NextJumpOffsetX;
    float NextJumpOffsetY;
    uint ShowNextJump;
};

float ShapeDistance(float2 centered) {
    const float2 shape_size = max(
        float2(ShapeWidth, ShapeHeight),
        float2(0.0001, 0.0001)
    );
    const float2 scaled = abs(centered) / shape_size;
    return lerp(
        max(scaled.x, scaled.y),
        length(scaled),
        saturate(ShapeRoundness)
    );
}

float4 LoadInputBilinear(float2 position) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 minimum = int2(InputBase);
    const int2 maximum = minimum + int2(InputSize) - 1;
    const int2 p00 = clamp(int2(base), minimum, maximum);
    const int2 p10 = clamp(p00 + int2(1, 0), minimum, maximum);
    const int2 p01 = clamp(p00 + int2(0, 1), minimum, maximum);
    const int2 p11 = clamp(p00 + int2(1, 1), minimum, maximum);
    return lerp(
        lerp(LowResolutionColor.Load(int3(p00, 0)),
             LowResolutionColor.Load(int3(p10, 0)), fraction.x),
        lerp(LowResolutionColor.Load(int3(p01, 0)),
             LowResolutionColor.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float2 InputPosition(uint2 local_pixel) {
    return float2(InputBase) +
        (float2(local_pixel) + 0.5) * float2(InputSize) /
        float2(OutputSize) - 0.5;
}

// The private texture exactly matches the DLSS reconstruction extent.
// Integrate pixel coverage, including fractional boundaries, without negative
// filter lobes or an extra intermediate pass. 1x retains the original load.
float4 LoadDlssResampled(uint2 local_pixel) {
    uint width, height;
    DlssColor.GetDimensions(width, height);
    const uint2 source_size = uint2(width, height) - DlssOrigin;
    if (all(source_size == RectSize)) {
        const int2 pixel = int2(DlssOrigin + local_pixel);
        return DlssColor.Load(int3(pixel, 0));
    }
    const float2 ratio = float2(source_size) / float2(RectSize);
    if (any(source_size < RectSize)) {
        const float2 position = (float2(local_pixel) + 0.5) * ratio - 0.5;
        const int2 base = int2(floor(position));
        const float2 fraction = frac(position);
        const int2 maximum = int2(source_size) - 1;
        const int2 p00 = int2(DlssOrigin) + clamp(base, int2(0, 0), maximum);
        const int2 p10 = int2(DlssOrigin) + clamp(base + int2(1, 0), int2(0, 0), maximum);
        const int2 p01 = int2(DlssOrigin) + clamp(base + int2(0, 1), int2(0, 0), maximum);
        const int2 p11 = int2(DlssOrigin) + clamp(base + int2(1, 1), int2(0, 0), maximum);
        return lerp(lerp(DlssColor.Load(int3(p00, 0)), DlssColor.Load(int3(p10, 0)), fraction.x),
            lerp(DlssColor.Load(int3(p01, 0)), DlssColor.Load(int3(p11, 0)), fraction.x), fraction.y);
    }

    const float2 begin = float2(local_pixel) * ratio;
    const float2 end = min(float2(local_pixel + 1U) * ratio, float2(source_size));
    float4 sum = 0.0;
    float total = 0.0;
    [loop] for (int y = int(floor(begin.y)); y < int(ceil(end.y)); ++y) {
        const float wy = max(0.0, min(end.y, float(y + 1)) - max(begin.y, float(y)));
        [loop] for (int x = int(floor(begin.x)); x < int(ceil(end.x)); ++x) {
            const float wx = max(0.0, min(end.x, float(x + 1)) - max(begin.x, float(x)));
            const int2 pixel = int2(DlssOrigin) + clamp(int2(x, y), int2(0, 0), int2(source_size) - 1);
            sum += DlssColor.Load(int3(pixel, 0)) * (wx * wy);
            total += wx * wy;
        }
    }
    return sum / max(total, 0.000001);
}

[numthreads(16, 16, 1)]
void CompositeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (any(dispatch_id.xy >= OutputSize)) return;
    const uint2 local_pixel = dispatch_id.xy;
    const uint2 output_pixel = OutputOrigin + local_pixel;
    const float4 bilinear = LoadInputBilinear(InputPosition(local_pixel));
    float2 centered =
        (float2(local_pixel) + 0.5) / (0.5 * float2(OutputSize)) - 1.0;
    centered.x -= ShapeOffsetX * (1.0 - ShapeWidth);
    centered.y -= ShapeOffsetY * (1.0 - ShapeHeight);
    const float distance_from_center = ShapeDistance(centered);
    const float2 pixel_size = 2.0 / float2(OutputSize);
    const float distance_per_pixel = max(
        abs(ShapeDistance(centered + float2(pixel_size.x, 0.0)) -
            distance_from_center),
        abs(ShapeDistance(centered + float2(0.0, pixel_size.y)) -
            distance_from_center)
    );
    if (ShowNextJump != 0U) {
        float2 next_centered = (float2(local_pixel) + 0.5) / (0.5 * float2(OutputSize)) - 1.0;
        next_centered -= float2(NextJumpOffsetX * (1.0 - ShapeWidth), NextJumpOffsetY * (1.0 - ShapeHeight));
        const float next_distance = ShapeDistance(next_centered);
        const float next_pixel_distance = max(
            abs(ShapeDistance(next_centered + float2(pixel_size.x, 0.0)) - next_distance),
            abs(ShapeDistance(next_centered + float2(0.0, pixel_size.y)) - next_distance));
        if (next_distance <= 1.0 && next_distance >= 1.0 - 5.0 * next_pixel_distance) {
            GameOutput[output_pixel] = float4(0.0, 1.0, 0.0, 1.0);
            return;
        }
    }
    const bool alignment_border = ShowAlignmentBorder != 0U &&
        distance_from_center <= 1.0 &&
        distance_from_center >= 1.0 - 5.0 * distance_per_pixel;
    if (alignment_border) {
        GameOutput[output_pixel] = float4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    const float normalized_feather = Feather /
        max(0.0001, min(ShapeWidth, ShapeHeight));
    const float weight = Feather <= 0.0
        ? (distance_from_center <= 1.0 ? 1.0 : 0.0)
        : 1.0 - smoothstep(
            max(0.0, 1.0 - normalized_feather),
            1.0,
            distance_from_center
        );
    const bool inside_rect = all(output_pixel >= RectBase) &&
        all(output_pixel < RectBase + RectSize);
    if (!inside_rect || weight <= 0.0) {
        GameOutput[output_pixel] = bilinear;
        return;
    }

    // The private DX11 DLSS feature writes a packed crop at scratch (0, 0).
    // RectBase is where that crop belongs in the game's full-resolution output.
    const float4 dlss = LoadDlssResampled(output_pixel - RectBase);
    GameOutput[output_pixel] = lerp(bilinear, dlss, weight);
}
)";

} // namespace cheeky::foveated_dlss
