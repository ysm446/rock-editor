// 地面反射（下半球が「地面反射」のとき）の輝度。惑星の地表を Lambert 面として、
// 空の照度と大気を通った太陽の直射で照らした輝度を 1 画素に書く。
// rock-editor の AtmosphereCloudLighting.hlsl から、雲の高さでの平均輝度と月を除いたもの。
#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint outputIndex; uint lutIndex; uint2 pad;
};
[numthreads(1,1,1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    AtmosphericParameters ground = settings;
    ground.altitude = 0;
    // 上半球の空の照度（余弦重み）。フィボナッチ螺旋の 64 方向。
    float3 skyIrradiance = 0;
    [loop] for (uint i = 0; i < 64; ++i) {
        float y = (i + 0.5) / 64.0, phi = i * 2.39996323, r = sqrt(1 - y * y);
        float3 direction = float3(r * cos(phi), y, r * sin(phi));
        skyIrradiance += AtmosphericSky(direction, ground, lutIndex) * y * (6.283185307 / 64);
    }
    float3 sun = AtmosphereSun(settings);
    float3 direct = AtmComputeSunTransmittance(sun, settings.density, settings.mie, 0) * max(sun.y, 0) * settings.illuminance;
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputIndex];
    output[uint2(0, 0)] = float4(settings.groundAlbedo * (skyIrradiance + direct) / 3.141592654, 1);
}
