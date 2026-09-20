// 背景として環境キューブマップを描く。
// 頂点バッファは使わず、SV_VertexID からフルスクリーン三角形を組み立てる。
// 深度は書かず、メッシュを描いたあとに LESS_EQUAL で残った画素だけを埋める。

#include "EnvCommon.hlsli"

struct SkyboxConstants
{
    float4x4 inverseViewProjection;

    float3 cameraPosition;
    float intensity;

    uint environmentIndex;
    // 引くミップ。小数を許すのは、ぼかしの強さを段の間で決められるようにするため。
    float mipLevel;
    float2 pad0;

    // 太陽の円盤（シーンの空のときだけ）。環境マップには太陽を入れないので、ここで解析的に足す。
    // sunRadiance は円盤の輝度（大気を通った照度 / 立体角）。0 なら描かない。
    float3 sunDirection;
    float sunAngularRadius;
    float3 sunRadiance;
    float pad1;
};

ConstantBuffer<SkyboxConstants> g_skybox : register(b1);

struct VsOutput
{
    float4 clipPosition : SV_Position;
    float3 direction    : DIRECTION;
};

VsOutput VsMain(uint vertexId : SV_VertexID)
{
    VsOutput output;

    // 3 頂点で画面全体を覆う三角形。
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    const float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);

    // 深度は遠クリップに置く。
    output.clipPosition = float4(ndc, 1.0f, 1.0f);

    float4 worldPosition = mul(g_skybox.inverseViewProjection, float4(ndc, 1.0f, 1.0f));
    worldPosition /= worldPosition.w;
    output.direction = worldPosition.xyz - g_skybox.cameraPosition;

    return output;
}

float4 PsMain(VsOutput input) : SV_Target
{
    TextureCube<float4> environment = ResourceDescriptorHeap[g_skybox.environmentIndex];
    const float3 direction = normalize(input.direction);
    float3 radiance =
        environment.SampleLevel(g_samplerLinearClamp, direction, g_skybox.mipLevel).rgb * g_skybox.intensity;
    if (g_skybox.sunAngularRadius > 0.0f)
    {
        const float angle = acos(clamp(dot(direction, normalize(g_skybox.sunDirection)), -1.0f, 1.0f));
        const float edge = fwidth(angle);
        const float disc = 1.0f - smoothstep(g_skybox.sunAngularRadius - edge, g_skybox.sunAngularRadius + edge, angle);
        radiance += g_skybox.sunRadiance * disc;
    }
    // RGBA16F の範囲を守る。
    return float4(min(radiance, 65000.0f), 1.0f);
}
