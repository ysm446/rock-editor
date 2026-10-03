// FBX から読んだモデルの描画。モデルプレビューの窓・アセットの帯のサムネイル（sceneMode = 0）と、
// ビューポートに置いたモデル（sceneMode = 1。線形 HDR を書き、道路と同じシャドウマップの影を受ける）。
// rock-editor の ModelPreview.hlsl から、インスタンス描画と大気を外したもの。
//
// **照らし方はビューポートと同じ**（適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。
// マテリアルの合成モードを見る。マスク抜きはしきい値未満を捨て、半透明は不透明度を A に入れて重ねる
// （パイプラインの合成は ModelPreview.cpp が切り替える）。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "ImpostorCommon.hlsli"
#include "LayerMaterial.hlsli"
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
    uint tonemapMode; uint sceneMode; uint mapUvSets;
    // インスタンスの行列のバッファ（SRV 番号）。0xFFFFFFFF なら world を使う。
    uint instanceBuffer;
    float4x4 world;
    // シーンの影。MeshPbr と同じく転置せずに入っているので mul(M, v) で読む。
    float4x4 view;
    float4x4 lightViewProjections[4];
    uint4 shadowIndices;
    float4 shadowSplits;
    float4 shadowBiases;
    float shadowTexelSize, shadowBlend, shadowNear; uint shadowCascadeCount;
    LayerMaterialData layerMaterial;
    // インスタンスの行列のバッファの中の、このまとまりの先頭（SV_InstanceID は描画ごとに 0 から数える）。
    uint instanceBase; uint3 instancePad;
    // インポスター（植生の最終段）。アトラスの SRV、撮った球の中心と半径（モデル空間）、方向数、半球か、影パスか。
    uint impostorColor, impostorNormal, impostorFrames, impostorFullSphere;
    float3 impostorCenter; float impostorRadius;
    uint impostorShadow; uint3 impostorPad;
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

PixelInput VsMain(VertexInput input, uint instance : SV_InstanceID)
{
    PixelInput output;
    // インスタンス描画（山グラフの岩）では、行列を構造化バッファから読む（C++ 側で転置して並べてある）。
    float4x4 worldMatrix = g_model.world;
    if (g_model.instanceBuffer != 0xFFFFFFFFu)
    {
        StructuredBuffer<float4x4> instances = ResourceDescriptorHeap[g_model.instanceBuffer];
        worldMatrix = instances[g_model.instanceBase + instance];
    }
    // 置いたモデルの倍率は均一なので、法線と接線も同じ行列で回して正規化すればよい。
    const float3 world = mul(float4(input.position, 1.0f), worldMatrix).xyz;
    output.clip = mul(float4(world, 1.0f), g_model.viewProjection);
    output.position = world;
    output.normal = mul(input.normal, (float3x3)worldMatrix);
    output.tangent = float4(mul(input.tangent.xyz, (float3x3)worldMatrix), input.tangent.w);
    output.uv = input.uv;
    output.uv2 = input.uv2;
    return output;
}

// アルファ抜きの判定。ミップはアルファも平均するので、遠くほど閾値を超える画素が減って葉が痩せる。
// ミップが 1 段進むごとにアルファを持ち上げ、見かけの被覆を保つ（terrain-graph と同じ式。インポスターの焼き込みも同じ補正）。
static const float kAlphaMipScale = 0.25f;
void ClipAlpha(float alpha, float lod)
{
    clip(alpha * (1.0f + max(lod, 0.0f) * kAlphaMipScale) - g_model.maskThreshold);
}
// 切り抜き（masked）。不透明度のマップがあればそれで、無ければベースカラーのアルファで抜く（植生の葉のカード）。
void ClipMasked(MapUv uvSets[2])
{
    if (g_model.opacityIndex != kInvalidTextureIndex)
    {
        const MapUv m = uvSets[(g_model.mapUvSets >> ROCK_MAP_OPACITY) & 1u];
        const float lod = MapLod(g_model.opacityIndex, m.deltaX, m.deltaY);
        ClipAlpha(SelectChannel(SampleMap(g_model.opacityIndex, m.uv, lod), UnpackChannel(g_model.mapChannels, ROCK_CHANNEL_SLOT_OPACITY)), lod);
    }
    else if (g_model.baseColorIndex != kInvalidTextureIndex)
    {
        const MapUv m = uvSets[(g_model.mapUvSets >> ROCK_MAP_BASE_COLOR) & 1u];
        const float lod = MapLod(g_model.baseColorIndex, m.deltaX, m.deltaY);
        ClipAlpha(SampleMap(g_model.baseColorIndex, m.uv, lod).a, lod);
    }
    else
    {
        clip(g_model.opacityValue - g_model.maskThreshold);
    }
}

float3 ShadeModel(float3 position, float3 normal, float3 viewDirection, float3 baseColor, float roughness, float metallic,
                  float ambientOcclusion);

// 影パス（切り抜きのある材質だけ）。深度だけを書くので色は返さない。
void PsShadow(PixelInput input)
{
    if (g_model.blendMode != kBlendMasked) return;
    MapUv uvSets[2];
    uvSets[0].uv = input.uv;
    uvSets[0].deltaX = ddx(input.uv);
    uvSets[0].deltaY = ddy(input.uv);
    uvSets[1].uv = input.uv2;
    uvSets[1].deltaX = ddx(input.uv2);
    uvSets[1].deltaY = ddy(input.uv2);
    ClipMasked(uvSets);
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
        if (g_model.blendMode == kBlendMasked)
        {
            ClipMasked(uvSets);
        }
        else
        {
            opacity = g_model.opacityValue;
            if (g_model.opacityIndex != kInvalidTextureIndex)
            {
                const MapUv m = MAP_UV(ROCK_MAP_OPACITY);
                opacity = SampleScalarMap(g_model.opacityIndex, ROCK_CHANNEL_SLOT_OPACITY, m.uv, m.deltaX, m.deltaY);
            }
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
    if (g_model.layerMaterial.count > 0) {
        const MapUv m = MAP_UV(ROCK_MAP_BASE_COLOR);
        const float footprint = max(length(m.deltaX), length(m.deltaY));
        const LayerMaterialSample mixed = EvaluateLayerMaterial(g_model.layerMaterial, m.uv, m.uv, footprint.xx,
            float2(1,1), float2(1,0), float2(0,1));
        baseColor = mixed.color; roughness = mixed.surface.x; metallic = mixed.surface.y; ambientOcclusion = mixed.surface.z;
        const float3 tangent = normalize(input.tangent.xyz - normalGeometric * dot(input.tangent.xyz, normalGeometric));
        normal = normalize(tangent * mixed.normal.x + cross(normalGeometric, tangent) * input.tangent.w * mixed.normal.y + normalGeometric * mixed.normal.z);
    }

    const float3 viewDirection = normalize(g_model.cameraPosition - input.position);
    const float3 radiance = ShadeModel(input.position, normal, viewDirection, baseColor, roughness, metallic, ambientOcclusion);
    const float alpha = g_model.blendMode == kBlendTranslucent ? saturate(opacity) : 1.0f;
    // シーンでは線形 HDR のまま返す（露出とトーンマップはレンダラの後段が掛ける）。
    if (g_model.sceneMode != 0u) return float4(radiance, alpha);
    const float3 color = LinearToSrgb(ApplyTonemap(radiance * g_model.exposure, g_model.tonemapMode));
    return float4(color, alpha);
}

// 陰影（ビューポートと同じ式）。メッシュとインポスターで共用する。
float3 ShadeModel(float3 position, float3 normal, float3 viewDirection, float3 baseColor, float roughness, float metallic,
                  float ambientOcclusion)
{
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_model.lightDirection);
    // 影は直接光にだけ掛ける（MeshPbr と同じ）。プレビューでは受けない。
    const float shadow = SampleCascadedShadow(position, dot(normal, lightDirection));
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
    return radiance;
}

// --- インポスター（植生の最終段。terrain-graph と同じ焼き方の画像を描く）-------------------------
// カメラを向く四角形 1 枚。ピクセルごとに、視線に近い 3 方向の画像それぞれの平面（中心を通り、その方向に垂直）へ
// 視線を当て、当たった位置の画素を重みで混ぜる。株の置き方（インスタンスの行列）は補間せずに渡し、
// ピクセルでは視線をモデル空間へ戻して画像を選ぶ。
struct ImpostorInput
{
    float4 clip : SV_POSITION;
    float3 position : POSITION;
    nointerpolation float3 origin : ORIGIN;
    nointerpolation float3 axisX : AXISX;
    nointerpolation float3 axisY : AXISY;
    nointerpolation float3 axisZ : AXISZ;
    nointerpolation float scale : SCALE;
};

ImpostorInput VsImpostor(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 kCorners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    float4x4 worldMatrix = g_model.world;
    if (g_model.instanceBuffer != 0xFFFFFFFFu)
    {
        StructuredBuffer<float4x4> instances = ResourceDescriptorHeap[g_model.instanceBuffer];
        worldMatrix = instances[g_model.instanceBase + instance];
    }
    ImpostorInput output;
    // 行列から軸（倍率込み）と原点を取り出す（倍率は均一）。
    output.axisX = mul(float4(1, 0, 0, 0), worldMatrix).xyz;
    output.axisY = mul(float4(0, 1, 0, 0), worldMatrix).xyz;
    output.axisZ = mul(float4(0, 0, 1, 0), worldMatrix).xyz;
    output.origin = mul(float4(0, 0, 0, 1), worldMatrix).xyz;
    output.scale = max(length(output.axisX), 1e-6f);
    const float3 center = mul(float4(g_model.impostorCenter, 1), worldMatrix).xyz;
    const float radius = g_model.impostorRadius * output.scale;
    const float3 toCamera = g_model.cameraPosition - center;
    const float cameraDistance = length(toCamera);
    float3 right, up;
    // 影パスは平行光の正射影。板を光源へ向け、半径ぶんだけ覆う。
    const bool shadow = g_model.impostorShadow != 0;
    ImpostorFrameBasis(shadow ? normalize(g_model.lightDirection) : toCamera / max(cameraDistance, 1e-4f), right, up);
    // 透視では球の輪郭が中心の平面上で半径より少し大きく見えるので、その分だけ広げる。
    const float extent = shadow ? radius
        : cameraDistance > radius * 1.01f ? radius * cameraDistance / sqrt(cameraDistance * cameraDistance - radius * radius)
                                          : radius * 8;
    output.position = center + (right * kCorners[vertex % 6].x + up * kCorners[vertex % 6].y) * extent;
    output.clip = mul(float4(output.position, 1), g_model.viewProjection);
    return output;
}

// 焼いた画像を 1 本の視線で引いた結果（モデル空間）。
struct ImpostorHit
{
    float coverage, roughness;
    float3 color;    // リニア
    float3 normal;   // モデル空間
    float3 surface;  // 焼いた深度から戻した表面の位置（モデル空間）
};
// eye を通り ray へ進む視線で引く。toViewer はマスを選ぶ向き（透視なら株の中心からカメラ、平行光なら光源の向き）。
ImpostorHit SampleImpostor(float3 eye, float3 ray, float3 toViewer)
{
    const float3 center = g_model.impostorCenter;
    const float radius = g_model.impostorRadius;
    const uint frames = g_model.impostorFrames;
    const bool fullSphere = g_model.impostorFullSphere != 0;
    const ImpostorFrames selected = SelectImpostorFrames(toViewer, frames, fullSphere);
    Texture2D<float4> colorMap = ResourceDescriptorHeap[g_model.impostorColor];
    Texture2D<float4> normalMap = ResourceDescriptorHeap[g_model.impostorNormal];
    float2 atlasUv[3];
    float3 hit[3], direction[3];
    bool inside[3];
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        direction[k] = ImpostorFrameDirection(selected.frame[k], frames, fullSphere);
        float3 right, up;
        ImpostorFrameBasis(direction[k], right, up);
        const float denominator = dot(ray, direction[k]);
        const float t = dot(center - eye, direction[k]) / (abs(denominator) > 1e-4f ? denominator : 1e-4f);
        hit[k] = eye + ray * t;
        const float3 local = hit[k] - center;
        const float2 tile = float2(0.5f + dot(local, right) / (2 * radius), 0.5f - dot(local, up) / (2 * radius));
        inside[k] = all(tile >= 0) && all(tile <= 1);
        atlasUv[k] = (float2(selected.frame[k]) + saturate(tile)) / float(frames);
    }
    // ミップは最も重いマスの座標の変化で決める（分岐の前に微分を取る）。
    const float lod = MapLod(g_model.impostorColor, ddx(atlasUv[0]), ddy(atlasUv[0]));
    ImpostorHit result;
    result.coverage = 0; result.roughness = 0; result.color = 0; result.normal = 0; result.surface = 0;
    float2 normalSum = 0;
    [unroll] for (uint k = 0; k < 3; ++k)
    {
        if (!inside[k]) continue;
        const float4 c = colorMap.SampleLevel(g_samplerLinearClamp, atlasUv[k], lod);
        const float4 n = normalMap.SampleLevel(g_samplerLinearClamp, atlasUv[k], lod);
        const float weight = selected.weight[k] * c.a;
        result.color += SrgbToLinear(c.rgb) * weight;
        normalSum += n.xy * weight;
        result.roughness += n.w * weight;
        // 深度は「中心を通る平面から、撮った向きへどれだけ手前か」（terrain-graph の ImpostorBake.hlsl）。
        result.surface += (hit[k] + direction[k] * (n.z - 0.5f) * 2 * radius) * weight;
        result.coverage += weight;
    }
    const float inverse = 1 / max(result.coverage, 1e-4f);
    result.color *= inverse;
    result.roughness *= inverse;
    result.surface *= inverse;
    result.normal = DecodeImpostorNormal(normalSum * inverse * 2 - 1);
    // メッシュのアルファ抜きと同じく、遠くで痩せないようミップ段に応じて持ち上げる。
    result.coverage *= 1 + max(lod, 0) * kAlphaMipScale;
    return result;
}
// 株の置き方（ImpostorInput）でワールドとモデル空間を行き来する。軸は倍率込み。
float3 ImpostorToModel(ImpostorInput input, float3 world)
{
    const float3 relative = world - input.origin;
    const float inverseScale2 = 1 / (input.scale * input.scale);
    return float3(dot(relative, input.axisX), dot(relative, input.axisY), dot(relative, input.axisZ)) * inverseScale2;
}
float3 ImpostorDirectionToModel(ImpostorInput input, float3 direction)
{
    return normalize(float3(dot(direction, input.axisX), dot(direction, input.axisY), dot(direction, input.axisZ)));
}
float3 ImpostorToWorld(ImpostorInput input, float3 model)
{
    return input.origin + input.axisX * model.x + input.axisY * model.y + input.axisZ * model.z;
}

float4 PsImpostor(ImpostorInput input) : SV_TARGET
{
    const float3 camera = ImpostorToModel(input, g_model.cameraPosition);
    const float3 target = ImpostorToModel(input, input.position);
    const ImpostorHit hit = SampleImpostor(camera, normalize(target - camera), normalize(camera - g_model.impostorCenter));
    clip(hit.coverage - 0.5f);
    const float3 normal = normalize(input.axisX * hit.normal.x + input.axisY * hit.normal.y + input.axisZ * hit.normal.z);
    // 影と環境光の高さは、板の上の点ではなく焼いた深度から戻した表面で引く（影を落とす側と揃える）。
    const float3 surface = ImpostorToWorld(input, hit.surface);
    // LOD の色分け（baseColorIndex を無効にして tint を入れる）ではその色で塗る。
    const float3 baseColor = g_model.baseColorIndex == kInvalidTextureIndex && g_model.baseColorTint.r + g_model.baseColorTint.g + g_model.baseColorTint.b < 2.99f
                                 ? g_model.baseColorTint : hit.color;
    const float3 radiance = ShadeModel(surface, normal, normalize(g_model.cameraPosition - input.position), baseColor, hit.roughness, 0, 1);
    if (g_model.sceneMode != 0u) return float4(radiance, 1);
    return float4(LinearToSrgb(ApplyTonemap(radiance * g_model.exposure, g_model.tonemapMode)), 1);
}

// 影パス。光の向きの平行な視線で引き、焼いた深度から戻した表面の深度を書く。
float PsImpostorShadow(ImpostorInput input) : SV_Depth
{
    const float3 toLight = ImpostorDirectionToModel(input, normalize(g_model.lightDirection));
    const float3 target = ImpostorToModel(input, input.position);
    const ImpostorHit hit = SampleImpostor(target + toLight * (4 * g_model.impostorRadius), -toLight, toLight);
    clip(hit.coverage - 0.5f);
    const float4 projected = mul(float4(ImpostorToWorld(input, hit.surface), 1), g_model.viewProjection);
    return saturate(projected.z / projected.w);
}
