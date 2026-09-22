// マテリアルプレビューのメッシュ描画。
// 出力はトーンマップ前の線形放射輝度で、露出は後段の TonemapPass で掛ける。
//
// 2 枚目のレンダーターゲットへマテリアル UV を書き出す。ペイントのブラシパスが
// 「画面のこの画素はマテリアルのどこか」を引くために使う。CPU へ読み戻さずに
// 済ませるため、ID バッファではなく UV をそのまま持たせている。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "EnvCommon.hlsli"

struct LayerContext
{
    uint4 layerBaseColorIndex;
    uint4 layerNormalIndex;
    uint4 layerSurfaceIndex;
    uint4 layerHeightIndex;
    uint4 layerWorldUv;
    float4 layerUvRepeat;
    uint4 layerHeightGate;
    float4 layerHeightGateThreshold;
    float4 layerHeightGateSoftness;
    uint4 layerBlendMode;
    uint roadMaskIndex;
    float layerBlendRange;
    uint roadUvAlongU;
    float displacementMeters;
    float2 roadMaskScale;
    float2 origin;
};

struct BoundaryConstants {
    uint mask; uint height; uint alongU; uint invertMask;
    float center; float acrossSign; float width; float repeat;
    float depth; float heightCenter; float pad0; float pad1;
};
struct AppliedConstants {
    uint4 maps;
    float4 axisX, axisY, axisZ;
    float3 offset; uint method;
    uint maskIndex; float maskValue, maskRepeat; uint maskFlags;
};

struct MeshConstants
{
    float4x4 viewProjection;
    // カメラ空間で法線を見るための、投影を掛ける前のビュー行列。
    float4x4 view;
    float4x4 model;
    float4x4 normalMatrix;

    float3 cameraPosition;
    uint uvCheckerIndex;

    float3 lightDirection;   // サーフェスから光源へ向かう方向
    float lightIlluminance;  // lux 相当

    float3 lightColor;
    float surfaceDepthBiasMeters;

    float3 baseColor;
    float roughness;

    float metallic;
    float iblIntensity;
    uint prefilteredMipCount;
    float additiveHeightMeters;

    uint irradianceIndex;    // irradiance キューブの SRV
    uint prefilteredIndex;   // プリフィルタ済みキューブの SRV
    uint brdfLutIndex;       // 環境 BRDF の LUT
    uint useMaterialTextures;  // 0 ならメッシュの単色マテリアルを使う

    // 合成結果のチャンネル（bindless）
    uint materialBaseColorIndex;
    uint materialNormalIndex;
    uint materialSurfaceIndex;
    uint materialHeightIndex;

    // ビューポートに何を出すか（0 = シェーディング結果）。ROCK_VIEW_* と一致させる。
    uint debugView;
    // ハイトを形状に反映する量。0 なら押し出さない。
    float displacementScale;
    float roadMetersPerUv;
    uint meshDisplayFlags;

    float4x4 lightViewProjections[4];
    uint4 shadowIndices;
    float4 shadowSplits;
    float4 shadowBiases;
    float shadowTexelSize;
    float shadowBlend;
    float shadowNear;
    uint shadowCascadeCount;

    // テセレーションの分割量を画面上の辺の長さから決めるために使う。
    // **シャドウパスでも本描画と同じ値を渡す。** 分割が違うと形がずれ、
    // 自分の影が自分に落ちて縞（シャドウアクネ）になる。
    float4x4 tessellationViewProjection;
    float2 viewportSize;
    float tessellationMaxFactor;
    // 1 辺をおよそ何ピクセルに保つか。小さいほど細かく割る。
    float tessellationTargetPixels;

    // 押し出しに使うハイト。displacementUseRoadUv が 1 なら、displacementHeightIndex の
    // テクスチャを道路 UV で読む（白線が道路面と同じ量だけ動く）。0 なら自分の材質のハイトを uv で読む。
    uint displacementHeightIndex;
    uint displacementUseRoadUv;
    // 不透明度の扱い。0 = 不透明、1 = マスク抜き、2 = 半透明。
    uint opacityMode;
    float opacityThreshold;

    // 道路のレイヤー（スロット 1〜4）。C++ の MeshConstants と同じ並び。
    uint4 layerBaseColorIndex;
    uint4 layerNormalIndex;
    uint4 layerSurfaceIndex;
    uint4 layerHeightIndex;
    uint4 layerWorldUv;
    float4 layerUvRepeat;
    uint roadMaskIndex;
    uint layerCount;
    float layerBlendRange;
    uint roadUvAlongU;
    float2 roadMaskScale;
    float roadUvMetersPerUv;
    uint shadeLayers;
    // 下地のハイトで絞る。0 = 使わない、1 = 下地の高い所、2 = 下地の低い所。
    uint4 layerHeightGate;
    float4 layerHeightGateThreshold;
    float4 layerHeightGateSoftness;
    // 混ぜ方。0 = マスクどおり、1 = ハイトで競合。
    uint4 layerBlendMode;
    float4 layerDisplacementMeters;
    uint connectionPrototype;
    uint connectionContextCount;
    float2 connectionHeightFade;
    LayerContext connectionContexts[7];
    float2 connectionSecondHeightFade;
    uint connectionRoadMixIndex;
    uint connectionEndPad;
    BoundaryConstants boundaries[16];
    uint boundaryControlIndex;
    float boundaryFrameSign;
    uint boundaryCount;
    float boundaryPad;
    float4 mappingAxisX;
    float4 mappingAxisY;
    float4 mappingAxisZ;
    float3 mappingOffset;
    uint mappingMethod;
    AppliedConstants applied[8];
    uint appliedCount; uint3 appliedPadding;
};


// 「ハイト（ローカル）」で周りの平均を取る半径（合成テクセル）と、
// 引いた差を 0〜1 へ伸ばす倍率。素材の凹凸が見える強さとして選んである。
static const float kLocalHeightRadiusTexels = 6.0f;
static const float kLocalHeightGain = 16.0f;

// ビューポートの表示モード。C++ 側の renderer::DebugView と一致させること。
#define ROCK_VIEW_SHADED          0
#define ROCK_VIEW_BASECOLOR       1
#define ROCK_VIEW_NORMAL_VIEW     2
#define ROCK_VIEW_NORMAL_WORLD    3
#define ROCK_VIEW_ROUGHNESS       4
#define ROCK_VIEW_METALLIC        5
#define ROCK_VIEW_AO              6
#define ROCK_VIEW_HEIGHT          7
#define ROCK_VIEW_HEIGHT_LOCAL    8
#define ROCK_VIEW_WIREFRAME       9
#define ROCK_VIEW_CLAY            10

ConstantBuffer<MeshConstants> g_mesh : register(b1);

static const uint kNoTextureIndex = 0xFFFFFFFFu;

// --- 道路のレイヤー -------------------------------------------------------
// docs/design/road-material-layers.md。道路 UV から道路座標（横位置, 実距離）を作り、
// スロットごとに道路 UV かワールド XZ でタイルを引き、道路空間マスクの重みとハイトで競合させる。

// 道路座標（m）。x = 列 0（Right 端）からの横距離、y = 始点からの実距離。
float2 RoadMetersFromUv(float2 roadUv)
{
    const float2 meters = roadUv * g_mesh.roadUvMetersPerUv;
    return (g_mesh.roadUvAlongU != 0u) ? meters.yx : meters;
}

LayerContext LegacyContext()
{
    LayerContext c = (LayerContext)0;
    c.layerBaseColorIndex = g_mesh.layerBaseColorIndex;
    c.layerNormalIndex = g_mesh.layerNormalIndex;
    c.layerSurfaceIndex = g_mesh.layerSurfaceIndex;
    c.layerHeightIndex = g_mesh.layerHeightIndex;
    c.layerWorldUv = g_mesh.layerWorldUv;
    c.layerUvRepeat = g_mesh.layerUvRepeat;
    c.layerHeightGate = g_mesh.layerHeightGate;
    c.layerHeightGateThreshold = g_mesh.layerHeightGateThreshold;
    c.layerHeightGateSoftness = g_mesh.layerHeightGateSoftness;
    c.layerBlendMode = g_mesh.layerBlendMode;
    c.roadMaskIndex = g_mesh.roadMaskIndex;
    c.layerBlendRange = g_mesh.layerBlendRange;
    c.roadUvAlongU = g_mesh.roadUvAlongU;
    c.roadMaskScale = g_mesh.roadMaskScale;
    return c;
}

float2 LayerUv(LayerContext c, uint slot, float2 meters, float3 worldPosition)
{
    const float repeat = max(c.layerUvRepeat[slot], 1e-3f);
    if (c.layerWorldUv[slot] != 0u)
    {
        return worldPosition.xz / repeat;
    }
    const float2 uv = meters / repeat;
    return ((c.roadUvAlongU & 1) != 0u) ? uv.yx : uv;
}

// スロット 1〜4 の被覆率。マスクが無ければスロット 1 だけ。
float4 LayerCoverage(LayerContext c, float2 meters)
{
    float4 weights = float4(1.0f, 0.0f, 0.0f, 0.0f);
    if (c.roadMaskIndex == kNoTextureIndex)
    {
        return weights;
    }
    Texture2D<float4> mask = ResourceDescriptorHeap[c.roadMaskIndex];
    const float3 coverage = mask.SampleLevel(g_samplerLinearClamp, meters * c.roadMaskScale, 0.0f).rgb;
    weights.y = (c.layerHeightIndex.y != kNoTextureIndex) ? coverage.x : 0.0f;
    weights.z = (c.layerHeightIndex.z != kNoTextureIndex) ? coverage.y : 0.0f;
    weights.w = (c.layerHeightIndex.w != kNoTextureIndex) ? coverage.z : 0.0f;
    weights.x = saturate(1.0f - (weights.y + weights.z + weights.w));
    return weights;
}

// 下地のハイトで被覆率を絞る。Road Mask が「だいたいこの辺」、下地の凹凸が「その中のどこ」。
// 下地（スロット 1）の重みは残りで埋め直す。
float4 ApplyHeightGate(LayerContext c, float4 coverage, float baseHeight)
{
    [unroll]
    for (uint slot = 1; slot < 4; ++slot)
    {
        const uint mode = c.layerHeightGate[slot];
        if (mode == 0u || coverage[slot] <= 0.0f)
        {
            continue;
        }
        const float softness = max(c.layerHeightGateSoftness[slot], 1e-3f);
        const float signedDelta = (mode == 1u) ? (baseHeight - c.layerHeightGateThreshold[slot])
                                               : (c.layerHeightGateThreshold[slot] - baseHeight);
        coverage[slot] *= saturate(signedDelta / softness + 0.5f);
    }
    coverage.x = saturate(1.0f - (coverage.y + coverage.z + coverage.w));
    return coverage;
}

bool AnyHeightGate(LayerContext c)
{
    return (c.layerHeightGate.y | c.layerHeightGate.z | c.layerHeightGate.w) != 0u;
}

// スロットの重み。
//   マスクどおり（layerBlendMode = 0）: 被覆率がそのまま重み。境界は下地とのハイト差 × ブレンド幅だけ崩す。
//   ハイトで競合（layerBlendMode = 1）: 被覆率をハイトに足して、最大からブレンド幅の範囲を混ぜる。
// マスクどおりのスロットが先に取り、残りを下地とハイト競合のスロットで分ける。
float4 LayerHeightBlend(LayerContext c, float4 coverage, float4 heights)
{
    float4 maskWeights = 0.0f;
    float4 heightCoverage = coverage;
    [unroll]
    for (uint slot = 1; slot < 4; ++slot)
    {
        if (c.layerBlendMode[slot] == 0u)
        {
            const float w = saturate(coverage[slot] + (heights[slot] - heights[0]) * c.layerBlendRange);
            maskWeights[slot] = w * step(1e-6f, coverage[slot]);
            heightCoverage[slot] = 0.0f;
        }
    }
    float maskTotal = maskWeights.y + maskWeights.z + maskWeights.w;
    if (maskTotal > 1.0f)
    {
        maskWeights /= maskTotal;
        maskTotal = 1.0f;
    }
    const float remaining = 1.0f - maskTotal;
    heightCoverage.x = saturate(1.0f - (heightCoverage.y + heightCoverage.z + heightCoverage.w));
    const float4 score = heights + heightCoverage;
    const float peak = max(max(score.x, score.y), max(score.z, score.w));
    float4 blend = max(score - peak + max(c.layerBlendRange, 1e-3f), 0.0f);
    blend *= step(1e-6f, heightCoverage);
    const float total = blend.x + blend.y + blend.z + blend.w;
    const float4 heightBlend = (total > 1e-5f) ? blend / total : float4(1.0f, 0.0f, 0.0f, 0.0f);
    return maskWeights + heightBlend * remaining;
}

float LayerHeightLevel(LayerContext c, uint slot, float2 uv)
{
    Texture2D<float> heightMap = ResourceDescriptorHeap[c.layerHeightIndex[slot]];
    return heightMap.SampleLevel(g_samplerAnisoWrap, uv, 0.0f);
}

// プリセット内の合成と、プリセット間の被覆を別々に評価する。
// 両面で同じ境界座標を使う。Uは道路側から沿道側、Vは道路沿いの実距離。
float4 BoundaryControl(float2 meters, uint slot) {
    if (g_mesh.boundaryControlIndex == kNoTextureIndex) return 0;
    Texture2D<float4> control = ResourceDescriptorHeap[g_mesh.boundaryControlIndex];
    return control.SampleLevel(g_samplerLinearClamp, float2((slot + 0.5f) / 8.0f, meters.y * g_mesh.roadMaskScale.y), 0);
}
float2 BoundaryUv(BoundaryConstants b, float2 meters) {
    float2 uv = float2(saturate((meters.x - b.center) * b.acrossSign / max(b.width, 0.02f) + 0.5f),
                      frac(meters.y / max(b.repeat, 0.05f)));
    return b.alongU != 0 ? uv.yx : uv;
}
float BoundaryEnvelope(BoundaryConstants b, float2 meters) {
    return 1 - smoothstep(0.4f, 0.5f, abs(meters.x - b.center) / max(b.width, 0.02f));
}
float BoundaryHeight(float2 meters) {
    float result = 0;
    [loop] for (uint i = 0; i < g_mesh.boundaryCount; ++i) {
        const float4 control = BoundaryControl(meters, i / 2);
        const BoundaryConstants b = g_mesh.boundaries[i];
        const float weight = control[(i % 2) * 2] * BoundaryEnvelope(b, meters);
        if (weight <= 0 || b.height == kNoTextureIndex || b.depth <= 0) continue;
        Texture2D<float4> map = ResourceDescriptorHeap[b.height];
        result += (map.SampleLevel(g_samplerLinearClamp, BoundaryUv(b, meters), 0).r - b.heightCenter) * 2 * b.depth * weight;
    }
    return result;
}
struct ConnectionMix { float values[7]; };
ConnectionMix ConnectionWeights(float2 meters)
{
    Texture2D<float4> mask = ResourceDescriptorHeap[g_mesh.roadMaskIndex];
    float4 coverage = saturate(mask.SampleLevel(g_samplerLinearClamp, meters * g_mesh.roadMaskScale, 0));
    if (g_mesh.connectionContextCount < 5) coverage.ba = 0;
    const float4 originalCoverage = coverage;
    float4 replacement = 0;
    float2 replaced = 0;
    [loop] for (uint boundary = 0; boundary < g_mesh.boundaryCount; ++boundary) {
        const uint side = boundary % 2;
        const float4 boundaryControl = BoundaryControl(meters, boundary / 2);
        const BoundaryConstants b = g_mesh.boundaries[boundary];
        // マスクは幅の外でも端の色を維持する。ここで減衰すると従来の混合が再び現れる。
        // 溝のハイトだけはBoundaryHeightで幅の外へ減衰させる。
        const float weight = boundaryControl[side * 2];
        if (weight <= 0 || b.mask == kNoTextureIndex) continue;
        Texture2D<float4> map = ResourceDescriptorHeap[b.mask];
        float maskValue = saturate(map.SampleLevel(g_samplerLinearClamp, BoundaryUv(b, meters), 0).r);
        if (b.invertMask != 0) maskValue = 1 - maskValue;
        const float second = boundaryControl[side * 2 + 1];
        replaced[side] += weight;
        replacement[side * 2] += (1 - maskValue) * (1 - second) * weight;
        replacement[side * 2 + 1] += (1 - maskValue) * second * weight;
    }
    coverage = originalCoverage * (1 - saturate(replaced.xxyy)) + replacement;
    const float base = saturate(1 - dot(coverage, 1.0f));
    const float total = max(base + dot(coverage, 1.0f), 1e-6f);
    ConnectionMix result = (ConnectionMix)0;
    result.values[0] = base / total;
    [unroll] for (uint i = 0; i < 4; ++i) result.values[i + 1] = coverage[i] / total;
    if (g_mesh.connectionContextCount == 7)
    {
        Texture2D<float4> roadMix = ResourceDescriptorHeap[g_mesh.connectionRoadMixIndex];
        const float2 mix = saturate(roadMix.SampleLevel(g_samplerLinearClamp, float2(0.5f, meters.y * g_mesh.roadMaskScale.y), 0).rg);
        const float3 roadWeights = float3(saturate(1 - mix.x - mix.y), mix);
        const float remaining = result.values[0] / max(dot(roadWeights, 1.0f), 1e-6f);
        result.values[0] = remaining * roadWeights.x;
        result.values[5] = remaining * roadWeights.y;
        result.values[6] = remaining * roadWeights.z;
    }
    return result;
}

float4 ContextWeights(LayerContext c, float2 meters, float3 worldPosition, out float4 heights)
{
    float4 coverage = LayerCoverage(c, meters);
    heights = 0.5f;
    heights[0] = LayerHeightLevel(c, 0, LayerUv(c, 0, meters, worldPosition));
    coverage = ApplyHeightGate(c, coverage, heights[0]);
    [unroll]
    for (uint slot = 1; slot < 4; ++slot)
    {
        if (coverage[slot] > 0 && c.layerHeightIndex[slot] != kNoTextureIndex)
            heights[slot] = LayerHeightLevel(c, slot, LayerUv(c, slot, meters, worldPosition));
    }
    return LayerHeightBlend(c, coverage, heights);
}

float2 ConnectionLocal(LayerContext c, float2 meters)
{
    float2 local = meters - c.origin;
    if ((c.roadUvAlongU & 2) != 0) local.x = -local.x;
    return local;
}

float ConnectionHeight(float2 meters, float3 worldPosition)
{
    const ConnectionMix weights = ConnectionWeights(meters);
    float height = 0;
    [loop]
    for (uint context = 0; context < g_mesh.connectionContextCount; ++context)
    {
        if (weights.values[context] <= 0) continue;
        const LayerContext c = g_mesh.connectionContexts[context];
        float4 heights;
        const float4 blend = ContextWeights(c, ConnectionLocal(c, meters), worldPosition, heights);
        height += weights.values[context] * dot(blend, heights - 0.5f) * c.displacementMeters;
    }
    if (g_mesh.connectionHeightFade.y > 0.0f)
    {
        const float t = saturate(abs(meters.x - g_mesh.connectionHeightFade.x) / g_mesh.connectionHeightFade.y);
        height *= t * t * (3.0f - 2.0f * t);
    }
    if (g_mesh.connectionSecondHeightFade.y > 0.0f)
    {
        const float t = saturate(abs(meters.x - g_mesh.connectionSecondHeightFade.x) / g_mesh.connectionSecondHeightFade.y);
        height *= t * t * (3.0f - 2.0f * t);
    }
    return 0.5f + height + BoundaryHeight(meters);
}

float3 WeightedDetailNormal(float3 detail, float weight)
{
    return normalize(float3(detail.xy * weight, 1 + (detail.z - 1) * weight));
}

void ConnectionShading(float2 meters, float3 worldPosition,
                       out float3 color, out float4 surface, out float3 normal)
{
    const ConnectionMix weights = ConnectionWeights(meters);
    color = 0;
    surface = 0;
    normal = float3(0, 0, 1);
    [loop]
    for (uint context = 0; context < g_mesh.connectionContextCount; ++context)
    {
        // 微分は分岐前に計算し、被覆境界でも同じLODを読む。
        const LayerContext c = g_mesh.connectionContexts[context];
        const float2 local = ConnectionLocal(c, meters);
        float2 uvs[4], dx[4], dy[4];
        [unroll]
        for (uint slot = 0; slot < 4; ++slot)
        {
            uvs[slot] = LayerUv(c, slot, local, worldPosition);
            dx[slot] = ddx(uvs[slot]);
            dy[slot] = ddy(uvs[slot]);
        }
        if (weights.values[context] <= 0) continue;
        float4 heights;
        const float4 blend = ContextWeights(c, local, worldPosition, heights);
        float3 contextNormal = float3(0, 0, 1);
        [unroll]
        for (uint layer = 0; layer < 4; ++layer)
        {
            if (blend[layer] <= 0 || c.layerBaseColorIndex[layer] == kNoTextureIndex) continue;
            Texture2D<float4> colorMap = ResourceDescriptorHeap[c.layerBaseColorIndex[layer]];
            Texture2D<float4> surfaceMap = ResourceDescriptorHeap[c.layerSurfaceIndex[layer]];
            Texture2D<float2> normalMap = ResourceDescriptorHeap[c.layerNormalIndex[layer]];
            const float weight = weights.values[context] * blend[layer];
            color += colorMap.SampleGrad(g_samplerAnisoWrap, uvs[layer], dx[layer], dy[layer]).rgb * weight;
            surface += surfaceMap.SampleGrad(g_samplerAnisoWrap, uvs[layer], dx[layer], dy[layer]) * weight;
            float3 detail = DecodeTangentNormal(normalMap.SampleGrad(g_samplerAnisoWrap, uvs[layer], dx[layer], dy[layer]));
            // UV軸を入れ替えたプリセットの法線を、共通断面の接空間へ揃える。
            if (c.layerWorldUv[layer] == 0 && (c.roadUvAlongU & 1) != 0) detail.xy = detail.yx;
            if (c.layerWorldUv[layer] == 0 && (c.roadUvAlongU & 4) != 0) detail.x = -detail.x;
            contextNormal = ReorientNormal(contextNormal, WeightedDetailNormal(detail, blend[layer]));
        }
        normal = ReorientNormal(normal, WeightedDetailNormal(contextNormal, weights.values[context]));
    }
    const float stepMeters = 0.002f;
    const float dxHeight = (BoundaryHeight(meters + float2(stepMeters, 0)) - BoundaryHeight(meters - float2(stepMeters, 0))) / (2 * stepMeters);
    const float dyHeight = (BoundaryHeight(meters + float2(0, stepMeters)) - BoundaryHeight(meters - float2(0, stepMeters))) / (2 * stepMeters);
    normal = ReorientNormal(normal, normalize(float3(-dxHeight * g_mesh.boundaryFrameSign, -dyHeight, 1)));
}

// 頂点 / ドメインシェーダ用。ブレンド後のハイト。
float BlendedHeightLevel(float2 roadUv, float3 worldPosition)
{
    const LayerContext c = LegacyContext();
    const float2 meters = RoadMetersFromUv(roadUv);
    if (g_mesh.connectionContextCount != 0) return ConnectionHeight(meters, worldPosition);
    float4 coverage = LayerCoverage(c, meters);
    float4 heights = 0.5f;
    // 下地のハイトは絞りに使うので、被覆率に関わらず先に読む。
    if (g_mesh.layerHeightIndex[0] != kNoTextureIndex)
    {
        heights[0] = LayerHeightLevel(c, 0, LayerUv(c, 0, meters, worldPosition));
        if (AnyHeightGate(c))
        {
            coverage = ApplyHeightGate(c, coverage, heights[0]);
        }
    }
    [unroll]
    for (uint slot = 1; slot < 4; ++slot)
    {
        if (coverage[slot] > 0.0f && g_mesh.layerHeightIndex[slot] != kNoTextureIndex)
        {
            heights[slot] = LayerHeightLevel(c, slot, LayerUv(c, slot, meters, worldPosition));
        }
    }
    const float4 blend = LayerHeightBlend(c, coverage, heights);
    if (g_mesh.connectionPrototype != 0u)
    {
        return 0.5f + dot(blend, (heights - 0.5f) * g_mesh.layerDisplacementMeters);
    }
    return dot(blend, heights);
}

// --- 合成結果のサンプリング ------------------------------------------------
// 道路は実距離UVを反復し、旧平面プレビューは端をクランプする。
float4 SampleMaterialColor(Texture2D<float4> map, float2 uv)
{
    if (g_mesh.roadMetersPerUv > 0.0f || g_mesh.mappingMethod == 1u) return map.Sample(g_samplerAnisoWrap, uv);
    return map.Sample(g_samplerAnisoClamp, uv);
}

float2 SampleMaterialNormal(Texture2D<float2> map, float2 uv)
{
    if (g_mesh.roadMetersPerUv > 0.0f || g_mesh.mappingMethod == 1u) return map.Sample(g_samplerAnisoWrap, uv);
    return map.Sample(g_samplerAnisoClamp, uv);
}

float SampleMaterialScalar(Texture2D<float> map, float2 uv)
{
    if (g_mesh.roadMetersPerUv > 0.0f || g_mesh.mappingMethod == 1u) return map.Sample(g_samplerAnisoWrap, uv);
    return map.Sample(g_samplerAnisoClamp, uv);
}

// 頂点 / ドメインシェーダ用（微分が無いので SampleLevel）。
float SampleMaterialScalarLevel(Texture2D<float> map, float2 uv)
{
    // 道路は実距離 UV でタイルを繰り返す。
    if (g_mesh.roadMetersPerUv > 0.0f) return map.SampleLevel(g_samplerAnisoWrap, uv, 0.0f);
    return map.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

struct VsInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
    float2 roadUv   : TEXCOORD1;
};

struct VsOutput
{
    float4 clipPosition  : SV_Position;
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
    float2 roadUv : TEXCOORD1;
};

// ライトから見た深度と比べて、この画素が影の中かを返す（1 = 当たっている）。
//
// 深度は普通の Texture2D として読む（比較サンプラは使わない）。
// 3x3 のポイントサンプルで平均を取り、境界のジャギーを和らげる。
float SampleShadow(float3 worldPosition, float nDotL, uint shadowIndex, float texelSize,
                   float bias, float4x4 lightViewProjection)
{
    if (shadowIndex == 0xFFFFFFFFu)
    {
        return 1.0f;
    }

    const float4 lightClip = mul(lightViewProjection, float4(worldPosition, 1.0f));
    if (lightClip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 ndc = lightClip.xyz / lightClip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    // 範囲の外は影を落とさない（シャドウマップが覆っていない）。
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    // 斜めに当たっているほど自己遮蔽しやすいので、下駄を増やす。
    const float slopeBias = bias * (1.0f + 3.0f * (1.0f - saturate(nDotL)));

    Texture2D<float> shadowMap = ResourceDescriptorHeap[NonUniformResourceIndex(shadowIndex)];
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) * texelSize;
            const float depth = shadowMap.SampleLevel(g_samplerPointClamp, uv + offset, 0.0f);
            visibility += (ndc.z - slopeBias <= depth) ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}

// カメラ前方距離で選択し、隣接する区間の重複範囲で混ぜる。
float SampleCascadedShadow(float3 worldPosition, float nDotL)
{
    if (g_mesh.shadowIndices.x == 0xFFFFFFFFu) return 1.0f;
    if (g_mesh.shadowCascadeCount == 1)
        return SampleShadow(worldPosition, nDotL, g_mesh.shadowIndices.x,
            g_mesh.shadowTexelSize, g_mesh.shadowBiases.x, g_mesh.lightViewProjections[0]);
    // 使うカスケードの数は 1〜4。最後の境界より遠くは影なし。
    const uint lastCascade = min(g_mesh.shadowCascadeCount, 4u) - 1u;
    const float distance = -mul(g_mesh.view, float4(worldPosition, 1.0f)).z;
    if (distance > g_mesh.shadowSplits[lastCascade]) return 1.0f;
    uint cascade = 0;
    while (cascade < lastCascade && distance > g_mesh.shadowSplits[cascade]) ++cascade;
    const float visibility = SampleShadow(worldPosition, nDotL, g_mesh.shadowIndices[cascade],
        g_mesh.shadowTexelSize, g_mesh.shadowBiases[cascade], g_mesh.lightViewProjections[cascade]);
    const float start = cascade == 0 ? g_mesh.shadowNear : g_mesh.shadowSplits[cascade - 1];
    const float end = g_mesh.shadowSplits[cascade];
    const float blendStart = end - (end - start) * g_mesh.shadowBlend;
    if (distance <= blendStart) return visibility;
    const uint nextCascade = min(cascade + 1, lastCascade);
    const float next = cascade < lastCascade ? SampleShadow(worldPosition, nDotL, g_mesh.shadowIndices[nextCascade],
        g_mesh.shadowTexelSize, g_mesh.shadowBiases[nextCascade], g_mesh.lightViewProjections[nextCascade]) : 1.0f;
    return lerp(visibility, next, smoothstep(blendStart, end, distance));
}

// --- ディスプレイスメント -------------------------------------------------
// 道路のレイヤーをブレンドした Height を読み、ワールド空間の法線方向へ押し引きする。
// **VsMain と DsMain の両方がこの関数を通る。** 別々の式を書くと、
// テセレーションの ON / OFF で形が変わってしまう。
// 頂点 / ドメインシェーダには微分が無いので SampleLevel を使う。
float3 ApplyDisplacement(float3 worldPosition, float3 worldNormal, float2 uv, float2 roadUv)
{
    float3 displaced = worldPosition;
    if (g_mesh.displacementScale != 0.0f && g_mesh.layerCount != 0u)
    {
        const float height = BlendedHeightLevel(roadUv, worldPosition);
        const float3 direction = (g_mesh.connectionPrototype != 0u) ? float3(0, 1, 0) : worldNormal;
        displaced += direction * ((height - 0.5f) * g_mesh.displacementScale);
    }
    // Decal自身のハイトは黒を基準に加算。UVは画像倍率適用後なので色・不透明度と一致する。
    if (g_mesh.additiveHeightMeters > 0.0f && g_mesh.useMaterialTextures != 0u)
    {
        Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
        displaced += worldNormal * (saturate(heightMap.SampleLevel(g_samplerAnisoWrap, uv, 0)) * g_mesh.additiveHeightMeters);
    }
    return displaced;
}

// D32の固定バイアスだけでは近接時の実寸余裕が不足する。
// 路面のハイトの値域を余裕に使い、投影深度だけを寄せる。帯の形状・UV・陰影位置は保持する。
float4 ProjectSurfacePosition(float3 worldPosition)
{
    float4 clip = mul(g_mesh.viewProjection, float4(worldPosition, 1.0f));
    if (g_mesh.surfaceDepthBiasMeters > 0.0f && clip.w > 0.0f)
    {
        const float3 toCamera = g_mesh.cameraPosition - worldPosition;
        const float distance = length(toCamera);
        const float offset = min(g_mesh.surfaceDepthBiasMeters, distance * 0.25f);
        const float3 biased = worldPosition + toCamera * (offset / max(distance, 1e-6f));
        const float4 projected = mul(g_mesh.viewProjection, float4(biased, 1.0f));
        if (projected.w > 0.0f) clip.z = max(0.0f, projected.z / projected.w) * clip.w;
    }
    return clip;
}

VsOutput VsMain(VsInput input)
{
    VsOutput output;

    const float3 worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    float3 worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    worldPosition = ApplyDisplacement(worldPosition, normalize(worldNormal), input.uv, input.roadUv);

    output.worldPosition = worldPosition;
    output.clipPosition = ProjectSurfacePosition(worldPosition);
    output.worldNormal = worldNormal;
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;
    output.roadUv = input.roadUv;

    return output;
}

// --- テセレーション -------------------------------------------------------
//
// 分割量は**画面上の辺の長さ**から決める。細かいメッシュではそのまま 1 になり、
// 近づいて 1 辺が伸びたときだけ細かく割る。ディスプレイスメントは
// ドメインシェーダで掛ける（分割後の点で高さを引くため）。

// ハードウェアの分割上限。
static const float kTessellationHardwareMax = 64.0f;

struct HsControlPoint
{
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
    float2 roadUv        : TEXCOORD1;
};

struct HsPatchConstants
{
    float edges[3]  : SV_TessFactor;
    float inside    : SV_InsideTessFactor;
};

// 投影も変位もせず、ワールド空間の制御点を出すだけ。
HsControlPoint VsControl(VsInput input)
{
    HsControlPoint output;
    output.worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    output.worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;
    output.roadUv = input.roadUv;
    return output;
}

// ワールド空間の 2 点が画面上で何ピクセル離れるか。
float ScreenEdgeFactor(float3 a, float3 b)
{
    const float4 clipA = mul(g_mesh.tessellationViewProjection, float4(a, 1.0f));
    const float4 clipB = mul(g_mesh.tessellationViewProjection, float4(b, 1.0f));
    // カメラの後ろに回った辺は判断できないので、最大まで割る。
    if (clipA.w <= 0.0f || clipB.w <= 0.0f)
    {
        return min(g_mesh.tessellationMaxFactor, kTessellationHardwareMax);
    }

    const float2 screenA = (clipA.xy / clipA.w) * 0.5f * g_mesh.viewportSize;
    const float2 screenB = (clipB.xy / clipB.w) * 0.5f * g_mesh.viewportSize;
    const float pixels = length(screenA - screenB);
    return clamp(pixels / max(g_mesh.tessellationTargetPixels, 1.0f), 1.0f,
                 min(g_mesh.tessellationMaxFactor, kTessellationHardwareMax));
}

HsPatchConstants HsConstant(InputPatch<HsControlPoint, 3> patch)
{
    HsPatchConstants output;
    // SV_TessFactor[i] は「制御点 i の向かい側の辺」に対応する。
    output.edges[0] = ScreenEdgeFactor(patch[1].worldPosition, patch[2].worldPosition);
    output.edges[1] = ScreenEdgeFactor(patch[2].worldPosition, patch[0].worldPosition);
    output.edges[2] = ScreenEdgeFactor(patch[0].worldPosition, patch[1].worldPosition);
    output.inside = (output.edges[0] + output.edges[1] + output.edges[2]) / 3.0f;
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HsConstant")]
HsControlPoint HsMain(InputPatch<HsControlPoint, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
VsOutput DsMain(HsPatchConstants patchConstants, float3 barycentric : SV_DomainLocation,
                const OutputPatch<HsControlPoint, 3> patch)
{
    VsOutput output;

    float3 worldPosition = patch[0].worldPosition * barycentric.x +
                           patch[1].worldPosition * barycentric.y +
                           patch[2].worldPosition * barycentric.z;
    const float3 worldNormal = normalize(patch[0].worldNormal * barycentric.x +
                                         patch[1].worldNormal * barycentric.y +
                                         patch[2].worldNormal * barycentric.z);
    const float3 worldTangent = patch[0].worldTangent * barycentric.x +
                                patch[1].worldTangent * barycentric.y +
                                patch[2].worldTangent * barycentric.z;
    const float2 uv = patch[0].uv * barycentric.x + patch[1].uv * barycentric.y +
                      patch[2].uv * barycentric.z;
    const float2 roadUv = patch[0].roadUv * barycentric.x + patch[1].roadUv * barycentric.y +
                          patch[2].roadUv * barycentric.z;

    // 分割後の点で高さを引いて押し出す。式は VsMain と共通の ApplyDisplacement。
    worldPosition = ApplyDisplacement(worldPosition, worldNormal, uv, roadUv);

    output.worldPosition = worldPosition;
    output.clipPosition = ProjectSurfacePosition(worldPosition);
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    output.tangentSign = patch[0].tangentSign;
    output.uv = uv;
    output.roadUv = roadUv;
    return output;
}

// UVチェッカーのタイル境界を画面微分でなだらかにする。
float GridLine(float2 coordinate)
{
    float2 footprint = max(fwidth(coordinate), 1e-5f);
    float2 distance = abs(frac(coordinate + 0.5f) - 0.5f);
    float2 coverage = 1.0f - smoothstep(0.4f, 1.4f, distance / footprint);
    coverage *= saturate(1.0f - footprint);
    return max(coverage.x, coverage.y);
}

// meshDisplayFlags のビット。C++ の kMeshFlag* と一致させる。
#define ROCK_MESH_FLAG_OUTLINE_HOVERED 4u
#define ROCK_MESH_FLAG_OUTLINE_SELECTED 8u

// ホバー / 選択メッシュのシルエット枠（外周の辺を LINELIST で描く）。
// 選択はライトギズモと同じ暖色、ホバーはワイヤーフレームと同じ寒色で区別する。
float4 PsOutline(VsOutput input) : SV_Target0
{
    if ((g_mesh.meshDisplayFlags & ROCK_MESH_FLAG_OUTLINE_SELECTED) != 0u)
        return float4(1.0f, 0.74f, 0.30f, 0.95f);
    return float4(0.55f, 0.85f, 1.0f, 0.9f);
}

// ワールド空間の投影フレーム。負側の面も右手系の接空間を保つ。
struct TriplanarFrame {
    float2 x, y, z;
    float3 weights, signs, normal;
};
TriplanarFrame MakeTriplanarFrame(float3 position, float3 normal)
{
    TriplanarFrame f;
    float3 p = position - g_mesh.mappingOffset;
    p = float3(dot(p, g_mesh.mappingAxisX.xyz), dot(p, g_mesh.mappingAxisY.xyz),
               dot(p, g_mesh.mappingAxisZ.xyz)) * g_mesh.mappingAxisX.w;
    f.normal = float3(dot(normal, g_mesh.mappingAxisX.xyz), dot(normal, g_mesh.mappingAxisY.xyz),
                      dot(normal, g_mesh.mappingAxisZ.xyz));
    f.signs = float3(f.normal.x < 0 ? -1 : 1, f.normal.y < 0 ? -1 : 1, f.normal.z < 0 ? -1 : 1);
    f.x = float2(-p.z * f.signs.x, p.y);
    f.y = float2(p.x, -p.z * f.signs.y);
    f.z = float2(p.x * f.signs.z, p.y);
    f.weights = pow(abs(f.normal), g_mesh.mappingAxisY.w);
    f.weights /= max(dot(f.weights, 1.0f.xxx), 1e-8f);
    return f;
}
float4 TriplanarColor(Texture2D<float4> map, TriplanarFrame f)
{
    return SampleMaterialColor(map, f.x) * f.weights.x + SampleMaterialColor(map, f.y) * f.weights.y
         + SampleMaterialColor(map, f.z) * f.weights.z;
}
float TriplanarHeight(Texture2D<float> map, TriplanarFrame f, float2 offset)
{
    return SampleMaterialScalar(map, f.x + offset) * f.weights.x + SampleMaterialScalar(map, f.y + offset) * f.weights.y
         + SampleMaterialScalar(map, f.z + offset) * f.weights.z;
}
float3 TriplanarNormal(Texture2D<float2> map, TriplanarFrame f)
{
    float3 nx = DecodeTangentNormal(SampleMaterialNormal(map, f.x));
    float3 ny = DecodeTangentNormal(SampleMaterialNormal(map, f.y));
    float3 nz = DecodeTangentNormal(SampleMaterialNormal(map, f.z));
    // 法線マップから勾配を取り、各投影の接線方向へ戻して表面へ投影する。
    // 平坦な法線マップでは勾配がゼロとなり、混合の鋭さによらず元の法線を保つ。
    float2 sx = nx.xy / max(nx.z, 0.05f);
    float2 sy = ny.xy / max(ny.z, 0.05f);
    float2 sz = nz.xy / max(nz.z, 0.05f);
    float3 slope = float3(0, sx.y, -sx.x * f.signs.x) * f.weights.x
                 + float3(sy.x, 0, -sy.y * f.signs.y) * f.weights.y
                 + float3(sz.x * f.signs.z, sz.y, 0) * f.weights.z;
    slope -= f.normal * dot(f.normal, slope);
    float3 n = normalize(f.normal + slope);
    return normalize(g_mesh.mappingAxisX.xyz * n.x + g_mesh.mappingAxisY.xyz * n.y + g_mesh.mappingAxisZ.xyz * n.z);
}

TriplanarFrame MakeAppliedFrame(AppliedConstants layer, float3 position, float3 normal)
{
    TriplanarFrame f;
    float3 p = position - layer.offset;
    p = float3(dot(p, layer.axisX.xyz), dot(p, layer.axisY.xyz),
               dot(p, layer.axisZ.xyz)) * layer.axisX.w;
    f.normal = float3(dot(normal, layer.axisX.xyz), dot(normal, layer.axisY.xyz),
                      dot(normal, layer.axisZ.xyz));
    f.signs = float3(f.normal.x < 0 ? -1 : 1, f.normal.y < 0 ? -1 : 1, f.normal.z < 0 ? -1 : 1);
    f.x = float2(-p.z * f.signs.x, p.y);
    f.y = float2(p.x, -p.z * f.signs.y);
    f.z = float2(p.x * f.signs.z, p.y);
    f.weights = pow(abs(f.normal), layer.axisY.w);
    f.weights /= max(dot(f.weights, 1.0f.xxx), 1e-8f);
    return f;
}
float3 AppliedNormal(AppliedConstants layer, Texture2D<float2> map, TriplanarFrame f)
{
    float3 nx = DecodeTangentNormal(map.Sample(g_samplerAnisoWrap, f.x));
    float3 ny = DecodeTangentNormal(map.Sample(g_samplerAnisoWrap, f.y));
    float3 nz = DecodeTangentNormal(map.Sample(g_samplerAnisoWrap, f.z));
    // 法線マップから勾配を取り、各投影の接線方向へ戻して表面へ投影する。
    // 平坦な法線マップでは勾配がゼロとなり、混合の鋭さによらず元の法線を保つ。
    float2 sx = nx.xy / max(nx.z, 0.05f);
    float2 sy = ny.xy / max(ny.z, 0.05f);
    float2 sz = nz.xy / max(nz.z, 0.05f);
    float3 slope = float3(0, sx.y, -sx.x * f.signs.x) * f.weights.x
                 + float3(sy.x, 0, -sy.y * f.signs.y) * f.weights.y
                 + float3(sz.x * f.signs.z, sz.y, 0) * f.weights.z;
    slope -= f.normal * dot(f.normal, slope);
    float3 n = normalize(f.normal + slope);
    return normalize(layer.axisX.xyz * n.x + layer.axisY.xyz * n.y + layer.axisZ.xyz * n.z);
}

struct AppliedValue { float3 color, normal; float4 surface; float height; };
AppliedValue EvaluateApplied(VsOutput input) {
    AppliedValue result;
    float3 n = normalize(input.worldNormal);
    float3 t = normalize(input.worldTangent - n * dot(input.worldTangent, n));
    float3 b = cross(n, t) * input.tangentSign;
    result.color = 0.18f.xxx; result.normal = n; result.surface = float4(.5,0,1,1); result.height = .5;
    for (uint i = 0; i < g_mesh.appliedCount; ++i) {
        AppliedConstants a = g_mesh.applied[i];
        Texture2D<float4> colorMap = ResourceDescriptorHeap[a.maps.x];
        Texture2D<float2> normalMap = ResourceDescriptorHeap[a.maps.y];
        Texture2D<float4> surfaceMap = ResourceDescriptorHeap[a.maps.z];
        Texture2D<float> heightMap = ResourceDescriptorHeap[a.maps.w];
        TriplanarFrame f = MakeAppliedFrame(a, input.worldPosition, n);
        float3 color; float4 surface; float height; float3 normal;
        if (a.method == 1u) {
            color = (colorMap.Sample(g_samplerAnisoWrap,f.x)*f.weights.x + colorMap.Sample(g_samplerAnisoWrap,f.y)*f.weights.y + colorMap.Sample(g_samplerAnisoWrap,f.z)*f.weights.z).rgb;
            surface = surfaceMap.Sample(g_samplerAnisoWrap,f.x)*f.weights.x + surfaceMap.Sample(g_samplerAnisoWrap,f.y)*f.weights.y + surfaceMap.Sample(g_samplerAnisoWrap,f.z)*f.weights.z;
            height = heightMap.Sample(g_samplerAnisoWrap,f.x)*f.weights.x + heightMap.Sample(g_samplerAnisoWrap,f.y)*f.weights.y + heightMap.Sample(g_samplerAnisoWrap,f.z)*f.weights.z;
            normal = AppliedNormal(a, normalMap, f);
        } else {
            color = SampleMaterialColor(colorMap,input.uv).rgb; surface = SampleMaterialColor(surfaceMap,input.uv);
            height = SampleMaterialScalar(heightMap,input.uv);
            float3 tn = DecodeTangentNormal(SampleMaterialNormal(normalMap,input.uv)); normal = normalize(t*tn.x+b*tn.y+n*tn.z);
        }
        float mask = a.maskValue;
        if (a.maskIndex != 0xffffffffu) {
            Texture2D<float4> m = ResourceDescriptorHeap[a.maskIndex];
            if ((a.maskFlags & 2u) != 0u) {
                float3 p = input.worldPosition / a.maskRepeat;
                float3 w = pow(abs(n), 4); w /= max(dot(w,1.0f.xxx),1e-8);
                mask *= m.Sample(g_samplerAnisoWrap,p.zy).r*w.x + m.Sample(g_samplerAnisoWrap,p.xz).r*w.y + m.Sample(g_samplerAnisoWrap,p.xy).r*w.z;
            } else mask *= m.Sample(g_samplerAnisoWrap,input.uv / a.maskRepeat).r;
        }
        if ((a.maskFlags & 1u) != 0u) mask = 1-mask;
        mask = saturate(mask);
        // ハイトで合成。マスクを基準に、この素材のハイトが下地より高い所を前に出す（C++ の HeightBlendWeight と同じ式）。
        if ((a.maskFlags & 4u) != 0u) {
            const float d = (height - result.height) * 0.5f + 0.5f;
            mask = saturate((d - (1.0f - mask)) / max(a.axisZ.w, 0.01f) + mask);
        }
        if ((a.maskFlags & 256u) != 0u) result.color = lerp(result.color,color,mask);
        if ((a.maskFlags & 512u) != 0u) result.normal = normalize(lerp(result.normal,normal,mask));
        if ((a.maskFlags & 1024u) != 0u) result.surface = lerp(result.surface,surface,mask);
        if ((a.maskFlags & 2048u) != 0u) result.height = lerp(result.height,height,mask);
    }
    return result;
}

// UV空間へラスタライズする。位置・法線は投影元のワールド座標のまま渡す。
VsOutput VsBake(VsInput input)
{
    VsOutput output = (VsOutput)0;
    output.clipPosition = float4(input.uv.x*2-1, 1-input.uv.y*2, 0, 1);
    output.worldPosition = input.position;
    output.worldNormal = input.normal;
    output.worldTangent = input.tangent.xyz;
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;
    return output;
}
float4 PsBake(VsOutput input) : SV_Target0
{
    if (g_mesh.appliedCount != 0u) {
        AppliedValue a = EvaluateApplied(input);
        if (g_mesh.debugView == 0u) return float4(LinearToSrgb(saturate(a.color)),1);
        if (g_mesh.debugView == 2u) return float4(a.surface.rgb,1);
        if (g_mesh.debugView == 3u) return float4(a.height.xxx,1);
        float3 n=normalize(input.worldNormal), t=normalize(input.worldTangent-n*dot(n,input.worldTangent));
        float3 b=cross(n,t)*input.tangentSign;
        return float4(normalize(float3(dot(a.normal,t),dot(a.normal,b),dot(a.normal,n)))*.5+.5,1);
    }
    Texture2D<float4> colorMap = ResourceDescriptorHeap[g_mesh.materialBaseColorIndex];
    Texture2D<float2> normalMap = ResourceDescriptorHeap[g_mesh.materialNormalIndex];
    Texture2D<float4> surfaceMap = ResourceDescriptorHeap[g_mesh.materialSurfaceIndex];
    Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
    const float3 n = normalize(input.worldNormal);
    const TriplanarFrame f = MakeTriplanarFrame(input.worldPosition,n);
    const bool tri = g_mesh.mappingMethod == 1u;
    if (g_mesh.debugView == 0u) {
        float3 color = tri ? TriplanarColor(colorMap,f).rgb : SampleMaterialColor(colorMap,input.uv).rgb;
        return float4(LinearToSrgb(saturate(color)),1);
    }
    if (g_mesh.debugView == 1u) {
        float3 tangentNormal;
        if (tri) {
            const float3 worldNormal = TriplanarNormal(normalMap,f);
            const float3 tangent = normalize(input.worldTangent-n*dot(input.worldTangent,n));
            const float3 bitangent = cross(n,tangent)*input.tangentSign;
            tangentNormal = normalize(float3(dot(worldNormal,tangent),dot(worldNormal,bitangent),dot(worldNormal,n)));
        } else tangentNormal = DecodeTangentNormal(SampleMaterialNormal(normalMap,input.uv));
        return float4(tangentNormal*0.5f+0.5f,1);
    }
    if (g_mesh.debugView == 2u) return float4((tri ? TriplanarColor(surfaceMap,f) : SampleMaterialColor(surfaceMap,input.uv)).rgb,1);
    const float height = tri ? TriplanarHeight(heightMap,f,0.0f.xx) : SampleMaterialScalar(heightMap,input.uv);
    return float4(height.xxx,1);
}

float4 PsMain(VsOutput input) : SV_Target0
{
    const bool uvChecker = (g_mesh.meshDisplayFlags & 2u) != 0u;
    const float3 geometricNormal = normalize(input.worldNormal);
    const float3 viewDirection = normalize(g_mesh.cameraPosition - input.worldPosition);

    float3 baseColor = g_mesh.baseColor;
    float roughnessValue = g_mesh.roughness;
    float metallicValue = g_mesh.metallic;
    float ambientOcclusion = 1.0f;
    float opacity = 1.0f;
    float3 normal = geometricNormal;

    // **クレイ表示**は、形（変位）はそのままで陰影だけをテクスチャ抜きにする。
    // 合成の色 / 法線 / サーフェスを読まず、単色マテリアルと面の向きで塗る。
    const bool clay = (g_mesh.debugView == ROCK_VIEW_CLAY);
    const bool useMaterialShading = (g_mesh.useMaterialTextures != 0u) && !clay && !uvChecker;
    const bool appliedOverrides = g_mesh.appliedCount != 0u && !uvChecker && !clay;

    if (uvChecker)
    {
        // UV確認用の拡散色だけを差し替える。照明・影・天球・露出は通常描画と共通。
        Texture2D<float4> checker = ResourceDescriptorHeap[g_mesh.uvCheckerIndex];
        baseColor = checker.Sample(g_samplerAnisoWrap, input.uv).rgb;
        roughnessValue = 0.8f;
        metallicValue = 0.0f;
    }

    if (clay)
    {
        // **面から法線を起こす。** 平面メッシュの頂点法線は押し出しても上を向いた
        // ままなので、そのまま陰影を付けると形が出ない。画面微分から取れば
        // 実際に描かれた三角形の向きになり、分割の粗さが面として見える
        // （メッシュの確認にはこれが要る）。
        const float3 faceNormal =
            normalize(cross(ddx(input.worldPosition), ddy(input.worldPosition)));
        // 三角形の巻き方によって裏返るので、視線の側へ向ける。
        normal = (dot(faceNormal, viewDirection) < 0.0f) ? -faceNormal : faceNormal;
    }

    if (useMaterialShading && g_mesh.shadeLayers != 0u && g_mesh.connectionContextCount != 0u)
    {
        float4 surface;
        float3 tangentNormal;
        ConnectionShading(RoadMetersFromUv(input.roadUv), input.worldPosition, baseColor, surface, tangentNormal);
        roughnessValue = surface.r;
        metallicValue = surface.g;
        ambientOcclusion = surface.b;
        const float3 tangent = normalize(input.worldTangent - geometricNormal * dot(geometricNormal, input.worldTangent));
        const float3 bitangent = cross(geometricNormal, tangent) * input.tangentSign;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y + geometricNormal * tangentNormal.z);
    }
    else if (useMaterialShading && g_mesh.shadeLayers != 0u)
    {
        const LayerContext c = LegacyContext();
        // 道路面。スロット 1〜4 を道路空間マスクの被覆率とハイトで競合させて混ぜる。
        const float2 meters = RoadMetersFromUv(input.roadUv);
        float4 coverage = LayerCoverage(c, meters);
        float2 uvs[4];
        float4 heights = 0.5f;
        [unroll]
        for (uint slot = 0; slot < 4; ++slot)
        {
            uvs[slot] = LayerUv(c, slot, meters, input.worldPosition);
        }
        // 下地のハイトは絞りに使うので、被覆率に関わらず先に読む。
        if (g_mesh.layerHeightIndex[0] != kNoTextureIndex)
        {
            Texture2D<float> baseHeightMap = ResourceDescriptorHeap[g_mesh.layerHeightIndex[0]];
            heights[0] = baseHeightMap.Sample(g_samplerAnisoWrap, uvs[0]);
            if (AnyHeightGate(c))
            {
                coverage = ApplyHeightGate(c, coverage, heights[0]);
            }
        }
        [unroll]
        for (uint slot = 1; slot < 4; ++slot)
        {
            if (coverage[slot] > 0.0f && g_mesh.layerHeightIndex[slot] != kNoTextureIndex)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.layerHeightIndex[slot]];
                heights[slot] = heightMap.Sample(g_samplerAnisoWrap, uvs[slot]);
            }
        }
        const float4 blend = LayerHeightBlend(c, coverage, heights);
        float3 blendedColor = 0.0f;
        float2 blendedNormal = 0.0f;
        float3 connectionNormal = float3(0, 0, 1);
        float4 blendedSurface = 0.0f;
        [unroll]
        for (uint slot2 = 0; slot2 < 4; ++slot2)
        {
            if (blend[slot2] <= 0.0f || g_mesh.layerBaseColorIndex[slot2] == kNoTextureIndex)
            {
                continue;
            }
            Texture2D<float4> baseColorMap = ResourceDescriptorHeap[g_mesh.layerBaseColorIndex[slot2]];
            Texture2D<float2> normalMap    = ResourceDescriptorHeap[g_mesh.layerNormalIndex[slot2]];
            Texture2D<float4> surfaceMap   = ResourceDescriptorHeap[g_mesh.layerSurfaceIndex[slot2]];
            blendedColor += baseColorMap.Sample(g_samplerAnisoWrap, uvs[slot2]).rgb * blend[slot2];
            blendedNormal += normalMap.Sample(g_samplerAnisoWrap, uvs[slot2]) * blend[slot2];
            if (g_mesh.connectionPrototype != 0u)
            {
                const float3 detail = DecodeTangentNormal(normalMap.Sample(g_samplerAnisoWrap, uvs[slot2]));
                // 共通の断面 UV にある法線を、重みに応じた傾きで RNM 合成する。
                const float3 weighted = normalize(float3(detail.xy * blend[slot2],
                    1.0f + (detail.z - 1.0f) * blend[slot2]));
                connectionNormal = ReorientNormal(connectionNormal, weighted);
            }
            blendedSurface += surfaceMap.Sample(g_samplerAnisoWrap, uvs[slot2]) * blend[slot2];
        }
        baseColor = blendedColor;
        roughnessValue = blendedSurface.r;
        metallicValue = blendedSurface.g;
        ambientOcclusion = blendedSurface.b;
        opacity = 1.0f;
        const float3 tangentNormal = (g_mesh.connectionPrototype != 0u) ? connectionNormal : DecodeTangentNormal(blendedNormal);
        const float3 tangent =
            normalize(input.worldTangent - geometricNormal * dot(geometricNormal, input.worldTangent));
        const float3 bitangent = cross(geometricNormal, tangent) * input.tangentSign;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                           geometricNormal * tangentNormal.z);
    }
    // Apply Material の合成結果は下で色・法線・Surface をすべて置き換える。基本材質の読み取り
    // （Triplanarなら9回の異方性サンプル）は捨てられるので省く。マスク抜きだけは基本材質の
    // 不透明度で discard するため、従来どおり読む。
    else if (useMaterialShading && (!appliedOverrides || g_mesh.opacityMode == 1u))
    {
        Texture2D<float4> baseColorMap = ResourceDescriptorHeap[g_mesh.materialBaseColorIndex];
        Texture2D<float2> normalMap    = ResourceDescriptorHeap[g_mesh.materialNormalIndex];
        Texture2D<float4> surfaceMap   = ResourceDescriptorHeap[g_mesh.materialSurfaceIndex];

        const float2 uv = input.uv;

        const bool triplanar = g_mesh.mappingMethod == 1u;
        const TriplanarFrame projection = MakeTriplanarFrame(input.worldPosition, geometricNormal);
        baseColor = triplanar ? TriplanarColor(baseColorMap, projection).rgb : SampleMaterialColor(baseColorMap, uv).rgb;

        const float4 surface = triplanar ? TriplanarColor(surfaceMap, projection) : SampleMaterialColor(surfaceMap, uv);
        roughnessValue = surface.r;
        metallicValue = surface.g;
        ambientOcclusion = surface.b;
        // 不透明度は Surface の A。マスク抜きはここで捨て、半透明は最後にアルファへ載せる。
        opacity = surface.a;
        if (g_mesh.opacityMode == 1u && opacity < g_mesh.opacityThreshold)
        {
            discard;
        }

        if (triplanar) {
            normal = TriplanarNormal(normalMap, projection);
        } else {
            const float3 tangentNormal = DecodeTangentNormal(SampleMaterialNormal(normalMap, uv));
            const float3 tangent =
                normalize(input.worldTangent - geometricNormal * dot(geometricNormal, input.worldTangent));
            const float3 bitangent = cross(geometricNormal, tangent) * input.tangentSign;
            normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                               geometricNormal * tangentNormal.z);
        }
    }

    if (appliedOverrides) {
        AppliedValue a = EvaluateApplied(input);
        baseColor=a.color; normal=a.normal; roughnessValue=a.surface.r; metallicValue=a.surface.g;
        ambientOcclusion=a.surface.b; opacity=a.surface.a;
    }
    // --- チャンネルを覗く表示 ----------------------------------------------
    // チャンネルの中身をそのまま出す。露出もトーンマップも掛けない
    // （後段の TonemapPass が素通しする）。**クレイはここへ来ない。**
    // 陰影を付ける表示なので、下のシェーディングをそのまま通す。
    if (g_mesh.debugView != ROCK_VIEW_SHADED && !clay)
    {
        float3 debugColor = float3(0.0f, 0.0f, 0.0f);
        if (g_mesh.debugView == ROCK_VIEW_BASECOLOR)
        {
            // ベースカラーはリニアで持っているので、見た目を合わせて sRGB で出す。
            debugColor = LinearToSrgb(saturate(baseColor));
        }
        else if (g_mesh.debugView == ROCK_VIEW_NORMAL_VIEW)
        {
            // 陰影に使う向きを**カメラ空間**で見る。ビュー行列は回転と平行移動だけ
            // なので、上 3x3 を掛ければ向きが移る（正規化は数値誤差の始末）。
            // カメラは -Z を向く（右手系）ので、正面を向いた面が +Z＝水色になり、
            // 法線マップと同じ読み方（平らなら水色）ができる。
            const float3 viewNormal = normalize(mul((float3x3)g_mesh.view, normal));
            debugColor = viewNormal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == ROCK_VIEW_NORMAL_WORLD)
        {
            // 陰影に実際に使う向き。法線マップを当てたあとのワールド空間法線。
            debugColor = normal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == ROCK_VIEW_ROUGHNESS)
        {
            debugColor = roughnessValue.xxx;
        }
        else if (g_mesh.debugView == ROCK_VIEW_METALLIC)
        {
            debugColor = metallicValue.xxx;
        }
        else if (g_mesh.debugView == ROCK_VIEW_AO)
        {
            debugColor = ambientOcclusion.xxx;
        }
        else if (g_mesh.debugView == ROCK_VIEW_WIREFRAME)
        {
            // 線だけを見る表示。塗りではないので単色で描く。
            debugColor = float3(0.66f, 0.72f, 0.78f);
        }
        else if (g_mesh.debugView == ROCK_VIEW_HEIGHT)
        {
            float height = 0.0f;
            if (g_mesh.connectionContextCount != 0u)
            {
                height = ConnectionHeight(RoadMetersFromUv(input.roadUv), input.worldPosition);
            }
            else if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                height = SampleMaterialScalar(heightMap, input.uv);
                if (g_mesh.mappingMethod == 1u)
                    height = TriplanarHeight(heightMap, MakeTriplanarFrame(input.worldPosition, geometricNormal), 0.0f.xx);
            }
            debugColor = saturate(height).xxx;
        }
        else if (g_mesh.debugView == ROCK_VIEW_HEIGHT_LOCAL)
        {
            // **その場の起伏だけ**を見る。地形の大きな高さ（標高差 600m の傾き）を
            // 周りの平均として引き、残りを 0.5 中心へ伸ばす。
            // 素材のハイトマップをそのまま貼ったような見た目になる。
            float local = 0.5f;
            if (g_mesh.connectionContextCount != 0u)
            {
                const float2 meters = RoadMetersFromUv(input.roadUv);
                const float center = ConnectionHeight(meters, input.worldPosition);
                float sum = 0;
                [unroll]
                for (int sampleIndex = 0; sampleIndex < 8; ++sampleIndex)
                {
                    const float angle = sampleIndex * 0.785398163f;
                    const float2 offset = float2(cos(angle), sin(angle)) * 0.02f;
                    sum += ConnectionHeight(meters + offset, input.worldPosition);
                }
                local = 0.5f + (center - sum / 8) * kLocalHeightGain;
            }
            else if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                const TriplanarFrame projection = MakeTriplanarFrame(input.worldPosition, geometricNormal);
                const float center = g_mesh.mappingMethod == 1u ? TriplanarHeight(heightMap, projection, 0.0f.xx)
                                                               : SampleMaterialScalar(heightMap, input.uv);

                // 周りの平均。半径は合成テクセル基準で固定する
                // （解像度を変えても「どのくらい大きな形を引くか」が変わらない）。
                float2 size = float2(1.0f, 1.0f);
                heightMap.GetDimensions(size.x, size.y);
                const float2 texel = 1.0f / max(size, float2(1.0f, 1.0f));
                const float radius = kLocalHeightRadiusTexels;
                float sum = 0.0f;
                [unroll]
                for (int i = 0; i < 8; ++i)
                {
                    const float angle = (float(i) / 8.0f) * 6.28318530718f;
                    const float2 offset = float2(cos(angle), sin(angle)) * radius * texel;
                    sum += g_mesh.mappingMethod == 1u ? TriplanarHeight(heightMap, projection, offset)
                                                     : SampleMaterialScalar(heightMap, input.uv + offset);
                }
                local = 0.5f + (center - sum / 8.0f) * kLocalHeightGain;
            }
            debugColor = saturate(local).xxx;
        }

        return float4(debugColor, 1.0f);
    }

    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallicValue, diffuseColor, f0);

    const float roughness = clamp(roughnessValue, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_mesh.lightDirection);
    // 影は直接光にだけ掛ける。環境光（IBL）は別に扱う。
    const float shadow = SampleCascadedShadow(input.worldPosition, dot(normal, lightDirection));

    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_mesh.lightColor, g_mesh.lightIlluminance,
                                            diffuseColor, f0, roughness) *
                      shadow;

    // --- IBL（分割和近似） -------------------------------------------------
    // saturate + 加算だと最大 1.00001 になり、FresnelSchlickRoughness の
    // pow(1 - nDotV, 5) が負の底で NaN になる。clamp で上限も守る。
    const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

    TextureCube<float4> irradianceMap = ResourceDescriptorHeap[g_mesh.irradianceIndex];
    TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_mesh.prefilteredIndex];
    Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_mesh.brdfLutIndex];

    // irradiance マップには E / pi（平均放射輝度）が入っているので、
    // diffuseColor を掛けるだけでよい。
    const float3 irradiance = irradianceMap.SampleLevel(g_samplerLinearClamp, normal, 0.0f).rgb;

    const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, roughness);
    const float3 kD = 1.0f - fresnel;
    const float3 diffuseIbl = kD * diffuseColor * irradiance;

    const float3 reflectionDirection = reflect(-viewDirection, normal);
    const float mipLevel = roughness * float(max(g_mesh.prefilteredMipCount, 1u) - 1u);
    const float3 prefiltered =
        prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;

    const float2 environmentBrdf =
        brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, roughness), 0.0f);
    const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

    radiance += (diffuseIbl + specularIbl) * g_mesh.iblIntensity * ambientOcclusion;

    // シーンカラーは R16G16B16A16_FLOAT。half の上限（65504）を超えると Inf になり、
    // トーンマップを経て NaN → ハイライト中心の黒点になる。上限手前でクランプする。
    return float4(min(radiance, 60000.0f),
                  (g_mesh.opacityMode == 2u) ? opacity : 1.0f);
}

// 描画と同じApplyDisplacementを使う検査。ハード法線の両側を独立して評価する。
struct ConnectionProbeConstants { uint inputIndex; uint outputIndex; uint edgeCount; uint samples; };
ConstantBuffer<ConnectionProbeConstants> g_probe : register(b0);
[numthreads(8, 8, 1)]
void CsConnectionProbe(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_probe.samples || id.y >= g_probe.edgeCount) return;
    Texture2D<float4> source = ResourceDescriptorHeap[g_probe.inputIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_probe.outputIndex];
    const float t = float(id.x) / float(g_probe.samples - 1);
    const float a = 1 - t;
    // DSの辺上の補間と同じ重み。両側の頂点・UV・法線を別々に読み込む。
    const float3 positionA = source.Load(int3(0, id.y, 0)).xyz * a + source.Load(int3(1, id.y, 0)).xyz * t;
    const float3 positionB = source.Load(int3(2, id.y, 0)).xyz * a + source.Load(int3(3, id.y, 0)).xyz * t;
    const float2 uvA = source.Load(int3(4, id.y, 0)).xy * a + source.Load(int3(5, id.y, 0)).xy * t;
    const float2 uvB = source.Load(int3(6, id.y, 0)).xy * a + source.Load(int3(7, id.y, 0)).xy * t;
    const float3 normalA = normalize(source.Load(int3(8, id.y, 0)).xyz * a + source.Load(int3(9, id.y, 0)).xyz * t);
    const float3 normalB = normalize(source.Load(int3(10, id.y, 0)).xyz * a + source.Load(int3(11, id.y, 0)).xyz * t);
    const float3 displacedA = ApplyDisplacement(positionA, normalA, uvA, uvA);
    const float3 displacedB = ApplyDisplacement(positionB, normalB, uvB, uvB);
    // 既知の1mm差も出力し、未実行・ゼロ埋めの読み戻しを合格と扱わない。
    const float control = length(displacedA - (displacedA + float3(0.001f, 0, 0)));
    output[id.xy] = float4(length(displacedA - displacedB), displacedA.y, displacedB.y, control);
}
