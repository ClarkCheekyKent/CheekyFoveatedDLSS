#pragma once
#include <cstdint>
namespace cheeky::foveated_dlss {
struct CodecConstants {
        std::uint32_t size[2];
        std::uint32_t source_size[2];
        std::uint32_t source_base[2];
        std::uint32_t proxy_size[2];
        float paper_white_scale;
        float transfer_strength;
        float color_strength;
        std::uint32_t hdr_mode;
        std::uint32_t region_base[2];
        std::uint32_t region_size[2];
        float foveation_width;
        float foveation_height;
        float foveation_roundness;
        float foveation_feather;
        std::uint32_t show_alignment_border;
        std::uint32_t mask_count, padding[2];
        float mask_bounds[4][4];
    };
    static_assert(sizeof(CodecConstants) == 40U * sizeof(std::uint32_t));
inline constexpr char nr_codec_shader[] = R"(
Texture2D<float4> Source0 : register(t0);
Texture2D<float4> Source1 : register(t1);
Texture2D<float4> Source2 : register(t2);
RWTexture2D<float4> Output0 : register(u0);
RWTexture2D<float4> Output1 : register(u1);

cbuffer CodecConstants : register(b0) {
    uint2 Size;
    uint2 SourceSize;
    uint2 SourceBase;
    uint2 ProxySize;
    float PaperWhiteScale;
    float TransferStrength;
    float ColorStrength;
    uint HdrMode;
    uint2 RegionBase;
    uint2 RegionSize;
    float FoveationWidth;
    float FoveationHeight;
    float FoveationRoundness;
    float FoveationFeather;
    uint ShowAlignmentBorder;
    uint MaskCount;
    uint2 MaskPadding;
    float4 MaskBounds[4];
};

float FoveationShapeDistance(float2 pixel) {
    if (MaskCount != 0) {
        const float2 uv = (pixel - float2(RegionBase) + 0.5) / max(float2(RegionSize), 1.0);
        float distance = 1e10;
        [unroll] for (uint i = 0; i < 4; ++i) if (i < MaskCount) {
            const float4 b = MaskBounds[i];
            const float2 p = abs(2.0 * uv - b.xy - b.zw) / max(b.zw - b.xy, 0.0001);
            distance = min(distance, lerp(max(p.x, p.y), length(p), saturate(FoveationRoundness)));
        }
        return distance;
    }
    const float2 centered =
        (pixel - float2(RegionBase) + 0.5) /
        (0.5 * max(float2(RegionSize), 1.0)) - 1.0;
    const float2 scaled = abs(centered);
    return lerp(
        max(scaled.x, scaled.y),
        length(scaled),
        saturate(FoveationRoundness)
    );
}

float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float SrgbEncodeChannel(float value) {
    value = saturate(value);
    return value <= 0.0031308
        ? 12.92 * value
        : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float3 SrgbEncode(float3 color) {
    return float3(
        SrgbEncodeChannel(color.r),
        SrgbEncodeChannel(color.g),
        SrgbEncodeChannel(color.b)
    );
}

float SrgbDecodeChannel(float value) {
    value = saturate(value);
    return value <= 0.04045
        ? value / 12.92
        : pow((value + 0.055) / 1.055, 2.4);
}

float3 SrgbDecode(float3 color) {
    return float3(
        SrgbDecodeChannel(color.r),
        SrgbDecodeChannel(color.g),
        SrgbDecodeChannel(color.b)
    );
}

float4 LoadSource0Bilinear(float2 position, uint2 origin, uint2 dimensions) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 minimum = int2(origin);
    const int2 maximum = minimum + int2(dimensions) - 1;
    const int2 p00 = clamp(int2(base), minimum, maximum);
    const int2 p10 = clamp(int2(base) + int2(1, 0), minimum, maximum);
    const int2 p01 = clamp(int2(base) + int2(0, 1), minimum, maximum);
    const int2 p11 = clamp(int2(base) + int2(1, 1), minimum, maximum);
    return lerp(
        lerp(Source0.Load(int3(p00, 0)), Source0.Load(int3(p10, 0)), fraction.x),
        lerp(Source0.Load(int3(p01, 0)), Source0.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float4 LoadSource1Bilinear(float2 position, uint2 dimensions) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 maximum = int2(dimensions) - 1;
    const int2 p00 = clamp(int2(base), int2(0, 0), maximum);
    const int2 p10 = clamp(int2(base) + int2(1, 0), int2(0, 0), maximum);
    const int2 p01 = clamp(int2(base) + int2(0, 1), int2(0, 0), maximum);
    const int2 p11 = clamp(int2(base) + int2(1, 1), int2(0, 0), maximum);
    return lerp(
        lerp(Source1.Load(int3(p00, 0)), Source1.Load(int3(p10, 0)), fraction.x),
        lerp(Source1.Load(int3(p01, 0)), Source1.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float4 LoadSource2Bilinear(float2 position, uint2 dimensions) {
    const float2 base = floor(position);
    const float2 fraction = position - base;
    const int2 maximum = int2(dimensions) - 1;
    const int2 p00 = clamp(int2(base), int2(0, 0), maximum);
    const int2 p10 = clamp(int2(base) + int2(1, 0), int2(0, 0), maximum);
    const int2 p01 = clamp(int2(base) + int2(0, 1), int2(0, 0), maximum);
    const int2 p11 = clamp(int2(base) + int2(1, 1), int2(0, 0), maximum);
    return lerp(
        lerp(Source2.Load(int3(p00, 0)), Source2.Load(int3(p10, 0)), fraction.x),
        lerp(Source2.Load(int3(p01, 0)), Source2.Load(int3(p11, 0)), fraction.x),
        fraction.y
    );
}

float3 UpgradeToneMap(float3 original, float3 proxy, float3 neural) {
    float original_y = Luminance(original);
    float proxy_y = Luminance(proxy);
    float neural_y = Luminance(neural);
    float ratio;
    if (original_y < proxy_y) {
        ratio = proxy_y > 0.0 ? original_y / proxy_y : 0.0;
    } else {
        float new_y = neural_y + max(0.0, original_y - proxy_y);
        ratio = neural_y > 0.0 ? new_y / neural_y : 0.0;
    }
    return lerp(original, max(neural * ratio, 0.0), TransferStrength);
}

[numthreads(16, 16, 1)]
void EncodeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (all(dispatch_id.xy < Size)) {
        Output0[dispatch_id.xy] = Source0.Load(int3(SourceBase + dispatch_id.xy, 0));
    }
    if (all(dispatch_id.xy < ProxySize)) {
        const float2 source_position =
            float2(SourceBase) +
            (float2(dispatch_id.xy) + 0.5) * float2(SourceSize) /
            float2(ProxySize) - 0.5;
        const float4 proxy_source = LoadSource0Bilinear(
            source_position,
            SourceBase,
            SourceSize
        );
        const float3 linear_color = max(
            proxy_source.rgb / max(PaperWhiteScale, 0.0001),
            0.0
        );
        const float3 encoded = HdrMode != 0 ? SrgbEncode(linear_color) : proxy_source.rgb;
        Output1[dispatch_id.xy] = float4(encoded, proxy_source.a);
    }
}

[numthreads(16, 16, 1)]
void BorderMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (any(dispatch_id.xy >= Size)) return;
    const float distance = FoveationShapeDistance(dispatch_id.xy);
    const float step = max(
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(1, 0)) - distance),
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(0, 1)) - distance));
    if (distance <= 1.0 && distance >= 1.0 - 5.0 * step)
        Output0[SourceBase + dispatch_id.xy] = float4(0, 1, 0, 1);
}

[numthreads(16, 16, 1)]
void DecodeMain(uint3 dispatch_id : SV_DispatchThreadID) {
    if (any(dispatch_id.xy >= Size)) return;
    const float4 original_sample = Source0.Load(int3(dispatch_id.xy, 0));
    const bool inside_region = all(dispatch_id.xy >= RegionBase) &&
        all(dispatch_id.xy < RegionBase + RegionSize);
    const float distance_from_center = FoveationShapeDistance(dispatch_id.xy);
    const float distance_per_pixel = max(
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(1.0, 0.0)) -
            distance_from_center),
        abs(FoveationShapeDistance(float2(dispatch_id.xy) + float2(0.0, 1.0)) -
            distance_from_center)
    );
    const bool alignment_border = ShowAlignmentBorder != 0U &&
        inside_region && distance_from_center <= 1.0 &&
        distance_from_center >= 1.0 - 5.0 * distance_per_pixel;
    if (alignment_border) {
        Output0[SourceBase + dispatch_id.xy] = float4(0.0, 1.0, 0.0, 1.0);
        return;
    }
    const float normalized_feather = FoveationFeather /
        max(0.0001, min(FoveationWidth, FoveationHeight));
    const float foveation_weight = FoveationFeather <= 0.0
        ? (distance_from_center <= 1.0 ? 1.0 : 0.0)
        : 1.0 - smoothstep(
            max(0.0, 1.0 - normalized_feather),
            1.0,
            distance_from_center
        );
    if (!inside_region || foveation_weight <= 0.0) {
        Output0[SourceBase + dispatch_id.xy] = original_sample;
        return;
    }
    const float2 proxy_position =
        (float2(dispatch_id.xy - RegionBase) + 0.5) * float2(ProxySize) /
        float2(RegionSize) - 0.5;
    const float4 proxy_sample = LoadSource1Bilinear(proxy_position, ProxySize);
    const float4 neural_sample = LoadSource2Bilinear(proxy_position, ProxySize);
    if (HdrMode == 0) {
        const float4 processed = float4(lerp(original_sample.rgb, neural_sample.rgb,
            TransferStrength * ColorStrength), original_sample.a);
        Output0[SourceBase + dispatch_id.xy] = lerp(original_sample, processed, foveation_weight);
        return;
    }
    const float3 original = max(
        original_sample.rgb / max(PaperWhiteScale, 0.0001),
        0.0
    );
    const float3 proxy = SrgbDecode(proxy_sample.rgb);
    const float3 neural = SrgbDecode(neural_sample.rgb);
    const float3 upgraded = UpgradeToneMap(original, proxy, neural);
    const float3 decoded = lerp(original, upgraded, ColorStrength) * PaperWhiteScale;
    const float4 processed = float4(max(decoded, 0.0), original_sample.a);
    Output0[SourceBase + dispatch_id.xy] = lerp(original_sample, processed, foveation_weight);
}
)";
}
