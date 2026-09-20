// レイヤー 1 枚ぶんを合成結果へ書く。
//
// 道路の材質は Surface 1 枚なので、レイヤーは自分のチャンネルへそのまま書く
// （下地との競合やマスクは無い）。一番下のレイヤーは全チャンネルを埋める。
//
// 出力の 4 枚は UAV として読み書きする。各スレッドは自分のテクセルしか触らないので、
// 同一ディスパッチ内での読み書きは安全。レイヤー間は UAV バリアで区切る。
//
// 出力タイル矩形と解像度を引数に取る形は崩さないこと（エクスポート時のタイル評価に必要）。

#include "CompositeCommon.hlsli"

#define TG_SOURCE_CONSTANT 0
#define TG_SOURCE_NOISE    1
#define TG_SOURCE_TEXTURE  2

// 法線マップの緑を反転して読む（OpenGL 規約の素材）。
#define TG_FLAG_FLIP_NORMAL_GREEN 0x1u

struct LayerConstants
{
    uint4 outputIndices;  // BaseColor, Normal, Surface, Height の UAV
    uint4 tile;           // x, y, width, height（出力全体の中での矩形）
    uint2 resolution;     // 出力全体の解像度
    uint channelMask;     // 書き込むチャンネルのビット
    uint flags;

    float4 baseColor;      // rgb
    float4 surfaceParams;  // roughness, metallic, ao, heightBase
    float4 heightParams;   // heightPerSize, uvScale, heightSource, 不透明度の定数
    // ハイトはノイズの amount を使わず、y に heightGain を入れる。
    float4 heightNoise;    // scale, heightGain, octaves, offset

    // 参照するテクスチャの SRV インデックス。kInvalidTextureIndex なら定数を使う。
    uint4 textureIndices0;  // baseColor, normal, roughness, metallic
    uint4 textureIndices1;  // ao, height, opacity, 未使用

    uint4 noiseTypes;   // height, 未使用 x3
    // スカラーのマップのチャンネル指定。4bit ずつ TG_CHANNEL_SLOT_* の順で詰めてある。
    uint4 mapChannels;  // x にすべて入る。yzw は未使用
    // ベースカラーの調整。マテリアルが持つ（ティントを掛けた**あと**に効く）。
    float4 colorAdjust;  // 色相（ラジアン）, 彩度, 明るさ, 未使用
};

ConstantBuffer<LayerConstants> g_layer : register(b1);

// コンピュートシェーダでは暗黙の LOD が使えないため、出力テクセル 1 つが張る
// UV 幅からミップレベルを求めて SampleLevel する。
float TextureLod(Texture2D<float4> texture, float uvPerOutputTexel)
{
    uint width = 0;
    uint height = 0;
    uint mipCount = 0;
    texture.GetDimensions(0, width, height, mipCount);

    const float texelsPerOutputTexel = max(float(width) * uvPerOutputTexel, 1.0f);
    return clamp(log2(texelsPerOutputTexel), 0.0f, float(max(mipCount, 1u) - 1u));
}

float4 SampleLayerTexture(uint index, float2 uv, float uvPerOutputTexel)
{
    Texture2D<float4> texture = ResourceDescriptorHeap[index];
    return texture.SampleLevel(g_samplerLinearWrap, uv, TextureLod(texture, uvPerOutputTexel));
}

// スカラーのマップを 1 つ読む。指定されたチャンネルだけを取り出す。
float SampleLayerScalar(uint index, uint channelSlot, float2 uv, float uvPerOutputTexel)
{
    const float4 sampled = SampleLayerTexture(index, uv, uvPerOutputTexel);
    return SelectChannel(sampled, UnpackChannel(g_layer.mapChannels.x, channelSlot));
}

// ハイトの基準面。ソースの値がこの値のとき、そのテクセルは基準の高さちょうどになる。
// ディスプレイスメントマップの「中間グレーが変位ゼロ」という慣習に合わせている。
// compositor::kHeightPivot と一致させること。
static const float kHeightPivot = 0.5f;

// h = 基準の高さ + (ソースの値 - 基準面) * 起伏の強さ。
// 基準面を挟むことで、起伏の強さを変えても平均の高さが動かない。
float SampleLayerHeight(float2 uv, float uvPerOutputTexel)
{
    const float base = g_layer.surfaceParams.w;
    const float gain = g_layer.heightNoise.y;

    const uint source = uint(g_layer.heightParams.z);
    if (source == TG_SOURCE_NOISE)
    {
        const float noise = SampleNoise(g_layer.noiseTypes.x, uv, g_layer.heightNoise.x,
                                        g_layer.heightNoise.w, int(g_layer.heightNoise.z));
        return base + (noise - kHeightPivot) * gain;
    }
    if (source == TG_SOURCE_TEXTURE && g_layer.textureIndices1.y != kInvalidTextureIndex)
    {
        // マテリアルのハイトマップはタイル素材なので wrap で読む。
        const float value = SampleLayerScalar(g_layer.textureIndices1.y, TG_CHANNEL_SLOT_HEIGHT,
                                              uv, uvPerOutputTexel);
        return base + (value - kHeightPivot) * gain;
    }

    // 定数。ソースの値がないので基準の高さそのもの。
    return base;
}

// ハイトの勾配からタンジェント空間法線を作る。解像度に依らない値になるよう、
// テクセル差ではなく UV 単位の微分を取る。
// 法線テクスチャが指定されている場合はそちらを使う。
//
// **勾配は実寸（m）で取る。** 強さのような無次元のつまみは持たない。
// ハイト 0〜1 の全幅が標高差（m）、出力 UV 0〜1 が地形の一辺（m）なので、
// heightParams.x = 標高差 / 一辺 を掛ければ d(高さ m) / d(距離 m) になる。
// UV スケールで模様を並べたぶんは同じだけ勾配が急になるので uvScale も掛ける。
float3 ComputeLayerNormal(float2 uv, float2 texelSize, float uvPerOutputTexel)
{
    if (g_layer.textureIndices0.y != kInvalidTextureIndex)
    {
        const float3 sampled =
            SampleLayerTexture(g_layer.textureIndices0.y, uv, uvPerOutputTexel).rgb;
        float3 tangentNormal = sampled * 2.0f - 1.0f;
        // **法線マップには 2 つの規約がある。**
        //   OpenGL : 緑 = 画像の上向き（−V）。Megascans などの既定
        //   DirectX: 緑 = 画像の下向き（+V）
        // このアプリの接空間と自前の法線は DirectX 規約なので、
        // OpenGL 規約のマップは緑を反転して読む（V 方向の陰影が逆になるため）。
        if ((g_layer.flags & TG_FLAG_FLIP_NORMAL_GREEN) != 0u)
        {
            tangentNormal.y = -tangentNormal.y;
        }
        return normalize(tangentNormal);
    }

    // 標高差 0 なら地形は平ら。勾配を取るまでもない。
    const float heightPerSize = g_layer.heightParams.x;
    if (heightPerSize <= 0.0f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }

    const float hx0 = SampleLayerHeight(uv - float2(texelSize.x, 0.0f), uvPerOutputTexel);
    const float hx1 = SampleLayerHeight(uv + float2(texelSize.x, 0.0f), uvPerOutputTexel);
    const float hy0 = SampleLayerHeight(uv - float2(0.0f, texelSize.y), uvPerOutputTexel);
    const float hy1 = SampleLayerHeight(uv + float2(0.0f, texelSize.y), uvPerOutputTexel);

    // UV 単位の勾配（合成解像度に依らない）。
    const float dx = (hx1 - hx0) * 0.5f / max(texelSize.x, 1e-6f);
    const float dy = (hy1 - hy0) * 0.5f / max(texelSize.y, 1e-6f);

    // 実寸の勾配へ。tan(傾き) がそのまま法線の xy になる。
    const float scale = heightPerSize * g_layer.heightParams.y;
    return normalize(float3(-dx * scale, -dy * scale, 1.0f));
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_layer.tile.z || dispatchThreadId.y >= g_layer.tile.w)
    {
        return;
    }

    const uint2 texel = g_layer.tile.xy + dispatchThreadId.xy;

    RWTexture2D<float4> baseColorTarget = ResourceDescriptorHeap[g_layer.outputIndices.x];
    RWTexture2D<float2> normalTarget    = ResourceDescriptorHeap[g_layer.outputIndices.y];
    RWTexture2D<float4> surfaceTarget   = ResourceDescriptorHeap[g_layer.outputIndices.z];
    RWTexture2D<float>  heightTarget    = ResourceDescriptorHeap[g_layer.outputIndices.w];

    const float2 texelSize = 1.0f / float2(g_layer.resolution);
    const float2 outputUv = (float2(texel) + 0.5f) * texelSize;
    const float2 uv = outputUv * g_layer.heightParams.y;
    const float2 noiseTexelSize = texelSize * g_layer.heightParams.y;

    // 出力テクセル 1 つが張る UV 幅。テクスチャのミップ選択に使う。
    const float uvPerOutputTexel = texelSize.x * g_layer.heightParams.y;

    // --- レイヤーの値 ------------------------------------------------------
    float3 layerBaseColor = g_layer.baseColor.rgb;
    float layerRoughness = g_layer.surfaceParams.x;
    float layerMetallic = g_layer.surfaceParams.y;
    float layerAo = g_layer.surfaceParams.z;

    if (g_layer.textureIndices0.x != kInvalidTextureIndex)
    {
        layerBaseColor *= SampleLayerTexture(g_layer.textureIndices0.x, uv, uvPerOutputTexel).rgb;
    }
    // 色相 / 彩度は**ティントを掛けたあと**に効かせる。順序を変えると、
    // 同じ設定でもティントの色に引きずられて結果が変わる。
    layerBaseColor =
        AdjustBaseColor(layerBaseColor, g_layer.colorAdjust.x, g_layer.colorAdjust.y, g_layer.colorAdjust.z);
    if (g_layer.textureIndices0.z != kInvalidTextureIndex)
    {
        layerRoughness = SampleLayerScalar(g_layer.textureIndices0.z,
                                           TG_CHANNEL_SLOT_ROUGHNESS, uv, uvPerOutputTexel);
    }
    if (g_layer.textureIndices0.w != kInvalidTextureIndex)
    {
        layerMetallic = SampleLayerScalar(g_layer.textureIndices0.w, TG_CHANNEL_SLOT_METALLIC,
                                          uv, uvPerOutputTexel);
    }
    if (g_layer.textureIndices1.x != kInvalidTextureIndex)
    {
        layerAo = SampleLayerScalar(g_layer.textureIndices1.x, TG_CHANNEL_SLOT_AO, uv,
                                    uvPerOutputTexel);
    }
    // 不透明度。マップが無ければ材質の定数。Surface の A へ書く。
    float layerOpacity = g_layer.heightParams.w;
    if (g_layer.textureIndices1.z != kInvalidTextureIndex)
    {
        layerOpacity = SampleLayerScalar(g_layer.textureIndices1.z, TG_CHANNEL_SLOT_OPACITY, uv,
                                         uvPerOutputTexel);
    }

    const float layerHeight = SampleLayerHeight(uv, uvPerOutputTexel);
    const float3 layerNormal = ComputeLayerNormal(uv, noiseTexelSize, uvPerOutputTexel);

    // --- 各チャンネルへ書く ------------------------------------------------
    if ((g_layer.channelMask & 0x1u) != 0u)
    {
        baseColorTarget[texel] = float4(layerBaseColor, 1.0f);
    }
    if ((g_layer.channelMask & 0x2u) != 0u)
    {
        normalTarget[texel] = EncodeTangentNormal(layerNormal);
    }
    if ((g_layer.channelMask & 0x4u) != 0u)
    {
        surfaceTarget[texel] = float4(layerRoughness, layerMetallic, layerAo, layerOpacity);
    }
    if ((g_layer.channelMask & 0x8u) != 0u)
    {
        heightTarget[texel] = layerHeight;
    }
}
