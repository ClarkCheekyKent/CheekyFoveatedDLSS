#pragma once
// Appended to cheeky_overlay_color_shader by the shader build script.
inline constexpr char vulkan_overlay_shader[] = R"(
[[vk::combinedImageSampler]] [[vk::binding(0,0)]] Texture2D<float4> Texture;
[[vk::combinedImageSampler]] [[vk::binding(0,0)]] SamplerState Sampler;
float4 main([[vk::location(0)]] float4 color:COLOR0, [[vk::location(1)]] float2 uv:TEXCOORD0):SV_Target {
    const float4 pixel=color*Texture.Sample(Sampler,uv);
    return float4(CheekyOverlayColor(pixel.rgb),pixel.a);
}
)";
