#ifndef TG_ATMOSPHERE_COMMON
#define TG_ATMOSPHERE_COMMON
// シーンの空（大気散乱）の共通部品。terrain-graph の AtmosphereCommon.hlsli から、雲・月・星空を除いたもの。
#include "EnvCommon.hlsli"
#include "AtmosphereScattering.hlsli"

// renderer::AtmosphereSettings と同じ並び。距離は m、角度はラジアン、太陽は大気圏外照度（lux）。
struct AtmosphericParameters {
    float azimuth; float elevation; float illuminance; float density;
    float mie; float eccentricity; float altitude; float groundAlbedo;
    uint lowerHemisphere; float3 padding;
};

float3 AtmosphereSun(AtmosphericParameters p) {
    return float3(cos(p.elevation) * sin(p.azimuth), sin(p.elevation), cos(p.elevation) * cos(p.azimuth));
}

// 視線方向の空の輝度（cd/m^2 相当）。groundRadiance は地面反射の輝度（下半球が「地面反射」のとき地表に当たるレイへ足す）。
float3 AtmosphericSky(float3 ray, AtmosphericParameters p, uint lutIndex, float3 groundRadiance = 0) {
    Texture2D<float4> lut = ResourceDescriptorHeap[lutIndex];
    // 「空の延長」は上半球を下へ折り返す見た目のための近似。背景と IBL で共用する。
    float3 sampleRay = ray;
    if (p.lowerHemisphere == 0 && ray.y < 0)
        sampleRay = normalize(float3(ray.x, max(-ray.y, 0.005), ray.z));
    float3 sky = AtmComputeScattering(sampleRay, AtmosphereSun(p), p.density, p.mie, p.eccentricity,
        lut, g_samplerLinearClamp, true, p.altitude, p.lowerHemisphere != 0 ? groundRadiance : 0, p.illuminance);
    if (p.lowerHemisphere == 0 && ray.y < 0)
        sky *= lerp(1, p.groundAlbedo, smoothstep(0, 0.08, -ray.y));
    return max(0, sky);
}
#endif
