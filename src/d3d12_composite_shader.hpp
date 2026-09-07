#pragma once

namespace cheeky::foveated_dlss {

inline constexpr char composite_shader_source[] = R"(
Texture2DArray<float4> LowResolutionColor : register(t0);
Texture2DArray<float4> DlssColor : register(t1);
RWTexture2DArray<float4> GameOutput : register(u0);

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
        lerp(LowResolutionColor.Load(int4(p00, 0, 0)),
             LowResolutionColor.Load(int4(p10, 0, 0)), fraction.x),
        lerp(LowResolutionColor.Load(int4(p01, 0, 0)),
             LowResolutionColor.Load(int4(p11, 0, 0)), fraction.x),
        fraction.y
    );
}

float2 InputPosition(uint2 local_pixel) {
    return float2(InputBase) +
        (float2(local_pixel) + 0.5) * float2(InputSize) /
        float2(OutputSize) - 0.5;
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
            GameOutput[uint3(output_pixel, 0)] = float4(0.0, 1.0, 0.0, 1.0);
            return;
        }
    }
    const bool alignment_border = ShowAlignmentBorder != 0U &&
        distance_from_center <= 1.0 &&
        distance_from_center >= 1.0 - 5.0 * distance_per_pixel;
    if (alignment_border) {
        GameOutput[uint3(output_pixel, 0)] = float4(1.0, 0.0, 0.0, 1.0);
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
        GameOutput[uint3(output_pixel, 0)] = bilinear;
        return;
    }
    const uint2 dlss_pixel = DlssOrigin + (output_pixel - RectBase);
    const float4 dlss = DlssColor.Load(int4(dlss_pixel, 0, 0));
    GameOutput[uint3(output_pixel, 0)] = lerp(bilinear, dlss, weight);
}
)";

}  // namespace cheeky::foveated_dlss
