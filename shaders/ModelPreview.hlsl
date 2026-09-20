// FBX から読んだモデルの描画。モデルプレビューの窓・アセットの帯のサムネイル（sceneMode = 0）と、
// ビューポートに置いたモデル（sceneMode = 1。線形 HDR を書き、道路と同じシャドウマップの影を受ける）。
// rock-editor の ModelPreview.hlsl から、インスタンス描画と大気を外したもの。
//
// **照らし方はビューポートと同じ**（適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。
// マテリアルの合成モードを見る。マスク抜きはしきい値未満を捨て、半透明は不透明度を A に入れて重ねる
// （パイプラインの合成は ModelPreview.cpp が切り替える）。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "EnvCommon.hlsli"
#include "Tonemap.hlsli"

// ModelPreview.cpp と同じ並び。単位は m、UV は読み込み時に画像座標へ変換済み。
struct ModelConstants
{
    float4x4 viewProjection;
    uint baseColorIndex, normalIndex, roughnessIndex, metallicIndex;
    uint aoIndex, opacityIndex, mapChannels, flipNormalGreen;
    uint irradianceIndex, prefilteredIndex, brdfLutIndex, prefilteredMipCount;
    float3 baseColorTint; float roughnessValue;
    float metallicValue, aoValue, opacityValue, maskThreshold;
    float2 colorAdjust; float brightness; uint blendMode;
    float3 cameraPosition; float exposure;
    float3 lightDirection; float lightIlluminance;
    float3 lightColor; float iblIntensity;
    uint tonemapMode; uint sceneMode; uint mapUvSets; uint pad;
    float4x4 world;
    // シーンの影。MeshPbr と同じく転置せずに入っているので mul(M, v) で読む。
    float4x4 view;
    float4x4 lightViewProjections[4];
    uint4 shadowIndices;
    float4 shadowSplits;
    float4 shadowBiases;
    float shadowTexelSize, shadowBlend, shadowNear; uint shadowCascadeCount;
};

ConstantBuffer<ModelConstants> g_model : register(b1);

// MaterialAsset::mapUvSets のビットの位置（compositor::MaterialMap と同じ並び）。
#define ROCK_MAP_BASE_COLOR 0u
#define ROCK_MAP_NORMAL     1u
#define ROCK_MAP_ROUGHNESS  2u
#define ROCK_MAP_METALLIC   3u
#define ROCK_MAP_AO         4u
#define ROCK_MAP_OPACITY    6u

static const uint kBlendMasked = 1u;
static const uint kBlendTranslucent = 2u;

float MapLod(uint index, float2 deltaX, float2 deltaY)
{
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    float2 dimensions;
    float levels;
    map.GetDimensions(0, dimensions.x, dimensions.y, levels);
    const float2 dx = deltaX * dimensions;
    const float2 dy = deltaY * dimensions;
    return 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8f));
}

float4 SampleMap(uint index, float2 uv, float lod)
{
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    return map.SampleLevel(g_samplerLinearWrap, uv, lod);
}

float SampleScalarMap(uint index, uint channelSlot, float2 uv, float2 deltaX, float2 deltaY)
{
    return SelectChannel(SampleMap(index, uv, MapLod(index, deltaX, deltaY)),
                         UnpackChannel(g_model.mapChannels, channelSlot));
}

// MeshPbr の SampleShadow / SampleCascadedShadow と同じ式。
float SampleShadow(float3 worldPosition, float nDotL, uint shadowIndex, float bias, float4x4 lightViewProjection)
{
    if (shadowIndex == kInvalidTextureIndex) return 1.0f;
    const float4 lightClip = mul(lightViewProjection, float4(worldPosition, 1.0f));
    if (lightClip.w <= 0.0f) return 1.0f;
    const float3 ndc = lightClip.xyz / lightClip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) return 1.0f;
    const float slopeBias = bias * (1.0f + 3.0f * (1.0f - saturate(nDotL)));
    Texture2D<float> shadowMap = ResourceDescriptorHeap[NonUniformResourceIndex(shadowIndex)];
    float visibility = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float depth = shadowMap.SampleLevel(g_samplerPointClamp, uv + float2(x, y) * g_model.shadowTexelSize, 0.0f);
            visibility += (ndc.z - slopeBias <= depth) ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}

float SampleCascadedShadow(float3 worldPosition, float nDotL)
{
    if (g_model.sceneMode == 0u || g_model.shadowIndices.x == kInvalidTextureIndex) return 1.0f;
    if (g_model.shadowCascadeCount == 1)
        return SampleShadow(worldPosition, nDotL, g_model.shadowIndices.x, g_model.shadowBiases.x, g_model.lightViewProjections[0]);
    // 使うカスケードの数は 1〜4。最後の境界より遠くは影なし。
    const uint lastCascade = min(g_model.shadowCascadeCount, 4u) - 1u;
    const float distance = -mul(g_model.view, float4(worldPosition, 1.0f)).z;
    if (distance > g_model.shadowSplits[lastCascade]) return 1.0f;
    uint cascade = 0;
    while (cascade < lastCascade && distance > g_model.shadowSplits[cascade]) ++cascade;
    const float visibility = SampleShadow(worldPosition, nDotL, g_model.shadowIndices[cascade],
        g_model.shadowBiases[cascade], g_model.lightViewProjections[cascade]);
    const float start = cascade == 0 ? g_model.shadowNear : g_model.shadowSplits[cascade - 1];
    const float end = g_model.shadowSplits[cascade];
    const float blendStart = end - (end - start) * g_model.shadowBlend;
    if (distance <= blendStart) return visibility;
    const uint nextCascade = min(cascade + 1, lastCascade);
    const float next = cascade < lastCascade ? SampleShadow(worldPosition, nDotL, g_model.shadowIndices[nextCascade],
        g_model.shadowBiases[nextCascade], g_model.lightViewProjections[nextCascade]) : 1.0f;
    return lerp(visibility, next, smoothstep(blendStart, end, distance));
}

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    // 2 つ目の UV（MeshVertex::roadUv の枠。無い FBX では uv と同じ）。
    float2 uv2 : TEXCOORD1;
};

struct PixelInput
{
    float4 clip : SV_POSITION;
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float2 uv2 : TEXCOORD1;
};

// マップ 1 つぶんを読む UV とその微分。マップごとに 1 つ目か 2 つ目の UV を選ぶ。
struct MapUv
{
    float2 uv, deltaX, deltaY;
};

PixelInput VsMain(VertexInput input)
{
    PixelInput output;
    // 置いたモデルの倍率は均一なので、法線と接線も同じ行列で回して正規化すればよい。
    const float3 world = mul(float4(input.position, 1.0f), g_model.world).xyz;
    output.clip = mul(float4(world, 1.0f), g_model.viewProjection);
    output.position = world;
    output.normal = mul(input.normal, (float3x3)g_model.world);
    output.tangent = float4(mul(input.tangent.xyz, (float3x3)g_model.world), input.tangent.w);
    output.uv = input.uv;
    output.uv2 = input.uv2;
    return output;
}

float4 PsMain(PixelInput input, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    MapUv uvSets[2];
    uvSets[0].uv = input.uv;
    uvSets[0].deltaX = ddx(input.uv);
    uvSets[0].deltaY = ddy(input.uv);
    uvSets[1].uv = input.uv2;
    uvSets[1].deltaX = ddx(input.uv2);
    uvSets[1].deltaY = ddy(input.uv2);
#define MAP_UV(map) uvSets[(g_model.mapUvSets >> (map)) & 1u]

    // --- 不透明度（合成モードが不透明なら見ない）--------------------------------
    float opacity = 1.0f;
    if (g_model.blendMode != 0u)
    {
        opacity = g_model.opacityValue;
        if (g_model.opacityIndex != kInvalidTextureIndex)
        {
            const MapUv m = MAP_UV(ROCK_MAP_OPACITY);
            opacity = SampleScalarMap(g_model.opacityIndex, ROCK_CHANNEL_SLOT_OPACITY, m.uv, m.deltaX, m.deltaY);
        }
        if (g_model.blendMode == kBlendMasked)
        {
            clip(opacity - g_model.maskThreshold);
            opacity = 1.0f;
        }
    }

    float3 baseColor = g_model.baseColorTint;
    if (g_model.baseColorIndex != kInvalidTextureIndex)
    {
        const MapUv m = MAP_UV(ROCK_MAP_BASE_COLOR);
        baseColor *= SampleMap(g_model.baseColorIndex, m.uv, MapLod(g_model.baseColorIndex, m.deltaX, m.deltaY)).rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_model.colorAdjust.x, g_model.colorAdjust.y, g_model.brightness);

    float roughness = g_model.roughnessValue;
    if (g_model.roughnessIndex != kInvalidTextureIndex)
    {
        const MapUv m = MAP_UV(ROCK_MAP_ROUGHNESS);
        roughness = SampleScalarMap(g_model.roughnessIndex, ROCK_CHANNEL_SLOT_ROUGHNESS, m.uv, m.deltaX, m.deltaY);
    }
    float metallic = g_model.metallicValue;
    if (g_model.metallicIndex != kInvalidTextureIndex)
    {
        const MapUv m = MAP_UV(ROCK_MAP_METALLIC);
        metallic = SampleScalarMap(g_model.metallicIndex, ROCK_CHANNEL_SLOT_METALLIC, m.uv, m.deltaX, m.deltaY);
    }
    float ambientOcclusion = g_model.aoValue;
    if (g_model.aoIndex != kInvalidTextureIndex)
    {
        const MapUv m = MAP_UV(ROCK_MAP_AO);
        ambientOcclusion = SampleScalarMap(g_model.aoIndex, ROCK_CHANNEL_SLOT_AO, m.uv, m.deltaX, m.deltaY);
    }

    // --- 法線 --------------------------------------------------------------
    // 半透明は両面を描くので、裏から見た面は法線を返す。
    const float faceSign = frontFace ? 1.0f : -1.0f;
    const float3 normalGeometric = normalize(input.normal) * faceSign;
    float3 normal = normalGeometric;
    if (g_model.normalIndex != kInvalidTextureIndex)
    {
        // 接線は 1 つ目の UV から求めてあるので、2 つ目の UV で読む法線マップは向きが合わないことがある。
        const MapUv m = MAP_UV(ROCK_MAP_NORMAL);
        float3 sampled = SampleMap(g_model.normalIndex, m.uv, MapLod(g_model.normalIndex, m.deltaX, m.deltaY)).rgb * 2.0f - 1.0f;
        if (g_model.flipNormalGreen != 0u)
        {
            sampled.y = -sampled.y;
        }
        const float3 tangent = normalize(input.tangent.xyz - normalGeometric * dot(input.tangent.xyz, normalGeometric));
        const float3 bitangent = cross(normalGeometric, tangent) * input.tangent.w;
        normal = normalize(tangent * sampled.x + bitangent * sampled.y + normalGeometric * sampled.z);
    }

    // --- 陰影（ビューポートと同じ式）---------------------------------------
    const float3 viewDirection = normalize(g_model.cameraPosition - input.position);
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_model.lightDirection);
    // 影は直接光にだけ掛ける（MeshPbr と同じ）。プレビューでは受けない。
    const float shadow = SampleCascadedShadow(input.position, dot(normal, lightDirection));
    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_model.lightColor, g_model.lightIlluminance,
                                            diffuseColor, f0, clampedRoughness) * shadow;

    if (g_model.irradianceIndex != kInvalidTextureIndex)
    {
        // MeshPbr と同じ分割和近似。nDotV は 1 を超えると NaN になるので clamp で守る。
        const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);
        TextureCube<float4> irradianceMap = ResourceDescriptorHeap[g_model.irradianceIndex];
        TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_model.prefilteredIndex];
        Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_model.brdfLutIndex];

        const float3 irradiance = irradianceMap.SampleLevel(g_samplerLinearClamp, normal, 0.0f).rgb;
        const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, clampedRoughness);
        const float3 diffuseIbl = (1.0f - fresnel) * diffuseColor * irradiance;

        const float3 reflectionDirection = reflect(-viewDirection, normal);
        const float mipLevel = clampedRoughness * float(max(g_model.prefilteredMipCount, 1u) - 1u);
        const float3 prefiltered = prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;
        const float2 environmentBrdf = brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, clampedRoughness), 0.0f);
        const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

        radiance += (diffuseIbl + specularIbl) * g_model.iblIntensity * ambientOcclusion;
    }

    const float alpha = g_model.blendMode == kBlendTranslucent ? saturate(opacity) : 1.0f;
    // シーンでは線形 HDR のまま返す（露出とトーンマップはレンダラの後段が掛ける）。
    if (g_model.sceneMode != 0u) return float4(radiance, alpha);
    const float3 color = LinearToSrgb(ApplyTonemap(radiance * g_model.exposure, g_model.tonemapMode));
    return float4(color, alpha);
}
