// シーンカラー（線形 HDR）に露出を掛け、トーンマップして sRGB で書き出す。

#include "Tonemap.hlsli"

struct TonemapConstants
{
    uint sourceIndex;
    uint outputIndex;
    uint width;
    uint height;
    float exposure;
    uint tonemapMode;
    // 0 以外なら、メッシュ側が書いた値をそのまま出す（チャンネルを覗く表示）。
    uint passthrough;
    // サムネイル撮影時だけ深度で背景を抜く。それ以外は未指定。
    uint backgroundDepthIndex;
};

ConstantBuffer<TonemapConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.width || dispatchThreadId.y >= g_constants.height)
    {
        return;
    }

    Texture2D<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    if (g_constants.backgroundDepthIndex != 0xffffffffu)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[g_constants.backgroundDepthIndex];
        if (depth[dispatchThreadId.xy] >= 1.0f)
        {
            output[dispatchThreadId.xy] = 0.0f;
            return;
        }
    }

    float3 color = source[dispatchThreadId.xy].rgb;

    // チャンネルを覗く表示は値そのものを見るためのものなので、
    // 露出もトーンマップも sRGB 変換も通さない。
    if (g_constants.passthrough != 0u)
    {
        output[dispatchThreadId.xy] = float4(color, 1.0f);
        return;
    }

    color *= g_constants.exposure;
    color = ApplyTonemap(color, g_constants.tonemapMode);

    output[dispatchThreadId.xy] = float4(LinearToSrgb(color), 1.0f);
}
