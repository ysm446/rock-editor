// シーンの空（大気散乱）を正距円筒の画像へ書く。Environment::BuildFromAtmosphere がこれをキューブマップと
// IBL（拡散・鏡面）にし、背景（スカイボックス）もそのキューブマップを引く。
// 太陽の円盤は直接光と二重に数えないよう、ここには描かない（背景のパスが解析的に足す）。
#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint outputIndex; uint lutIndex; uint groundIndex; uint width;
    uint height; uint3 pad;
};
[numthreads(8,8,1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[outputIndex];
    Texture2D<float4> ground = ResourceDescriptorHeap[groundIndex];
    const float3 ray = EquirectUvToDirection((id.xy + 0.5) / float2(width, height));
    output[id.xy] = float4(AtmosphericSky(ray, settings, lutIndex, ground.Load(int3(0, 0, 0)).rgb), 1);
}
