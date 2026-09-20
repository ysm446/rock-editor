#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>

namespace tg::compositor {

// 合成対象のチャンネル。出力テクスチャの構成と対応する。
enum class Channel : uint32_t {
    BaseColor = 0,
    Normal = 1,
    Surface = 2,  // R: Roughness, G: Metallic, B: AO
    Height = 3,
    Count = 4,
};

inline constexpr uint32_t ChannelBit(Channel channel) {
    return 1u << static_cast<uint32_t>(channel);
}

inline constexpr uint32_t kAllChannelBits = 0xFu;

// レイヤーの値のソース。
enum class ValueSource : uint32_t {
    Constant = 0,
    Noise = 1,
    Texture = 2,
};

// ハイトの基準面。ソースの値がこの値のとき、そのテクセルは「基準の高さ」ちょうどになる。
//
// ディスプレイスメントマップは「中間グレーが変位ゼロ」という慣習で作られるため、
// マップ自身の平均ではなく 0.5 を固定で使う。goals.md の「業界標準に合わせる」に従う。
// シェーダの kHeightPivot と一致させること。
inline constexpr float kHeightPivot = 0.5f;

// テクスチャの ID。0 は「なし」。TextureLibrary が払い出す。
using TextureId = uint32_t;
inline constexpr TextureId kNoTexture = 0;

// マテリアルの ID。0 は「なし」。MaterialLibrary が払い出す。
using MaterialAssetId = uint32_t;
inline constexpr MaterialAssetId kNoMaterialAsset = 0;

// シェーダへ渡す「参照しない」を表すインデックス。
inline constexpr uint32_t kInvalidTextureIndex = 0xFFFFFFFFu;

// テクスチャのどのチャンネルを読むか。シェーダの SelectChannel と一致させること。
//
// Megascans の `_ORD` のように、1 枚のテクスチャへ複数のマップを詰めたものがある
// （O = Occlusion / R = Roughness / D = Displacement）。
// スカラーのマップはどれも「テクスチャ + チャンネル」で指定する。
enum class TextureChannel : uint32_t {
    R = 0,
    G = 1,
    B = 2,
    A = 3,
};

// スカラーのマップ 1 つぶん。
struct MapSlot {
    TextureId texture = kNoTexture;
    TextureChannel channel = TextureChannel::R;
};

// チャンネル指定をまとめてシェーダへ渡すための詰め方。4bit ずつ、最大 8 スロット。
// 並びはシェーダの TG_CHANNEL_* と一致させること。
inline constexpr uint32_t PackChannel(TextureChannel channel, uint32_t slotIndex) {
    return static_cast<uint32_t>(channel) << (slotIndex * 4u);
}

// ノイズの種類。シェーダの TG_NOISE_* と一致させること。
// **並びを変えないこと。** プロジェクトには名前で保存するが、シェーダへは
// 数値で渡すので、シェーダの TG_NOISE_* と一致している必要がある。
enum class NoiseType : uint32_t {
    Fbm = 0,     // 一般的なフラクタルノイズ（値ノイズ）
    Ridged = 1,  // 尾根状。稜線や割れ目に向く
    Worley = 2,  // セル状。石畳や砂利に向く
    // 勾配ノイズ（Perlin）。値ノイズより滑らかで、方向のあるうねりになる。
    Perlin = 3,
    // 雲状（billow）。勾配ノイズの絶対値。丸い塊が寄り集まった見た目。
    Billow = 4,
    // 割れ目（Worley の F2 − F1）。セルの境目が明るくなる。
    Cracks = 5,
};

// フラクタルノイズのパラメータ。ハイトで使う。
struct NoiseParams {
    NoiseType type = NoiseType::Fbm;
    float scale = 6.0f;    // UV に掛ける周波数
    float amount = 1.0f;   // 出力への寄与
    int octaves = 5;
    float offset = 0.0f;   // 同じレイヤー内で別パターンにしたいときにずらす
};

// 1 レイヤーぶんの設定（Surface ノード 1 つ）。
//
// 道路の材質は Surface 1 枚で、マテリアル（PBR のマップ一式）を指す入れ物。
// 旧地形の積み重ね（下地との競合・マスク・加工）は撤去した。
struct MaterialLayer {
    std::string name = "Layer";
    bool enabled = true;

    // このレイヤーが書き込むチャンネル（Mixer と同じくチャンネル単位で切り替えられる）
    uint32_t channelMask = kAllChannelBits;

    // **リニアで持つ。既定は 18% グレー（0.18）。**
    // 写真と CG で共通の中間グレー基準で、画面上では sRGB 0.46 に見える。
    // 地面素材のアルベド（0.1〜0.3）にも収まる。
    // 0.5 をリニアで置くと sRGB 0.735 相当となり、コンクリートより明るくなる。
    DirectX::XMFLOAT3 baseColor = {0.18f, 0.18f, 0.18f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ambientOcclusion = 1.0f;

    // ハイト。基準の高さに、ソースの値を kHeightPivot 基準で振れさせたぶんを足す。
    //
    //   定数          : h = heightBase
    //   ノイズ / 画像 : h = heightBase + (src - kHeightPivot) * heightGain
    //
    // heightGain を変えても平均の高さは heightBase のまま動かないので、
    // 「どこに座るか」と「どれだけ起伏するか」を独立に決められる。
    // ハイトでは NoiseParams::amount を使わない（heightGain がその役目を担う）。
    // 画像のハイトはマテリアルのハイトマップから引く（同じ意味の値を 2 か所に置かない）。
    // 道路の材質では Road.cpp が「画像 / 基準 0.5 / 起伏 1.0」へ揃える。
    ValueSource heightSource = ValueSource::Noise;
    float heightBase = 0.5f;
    float heightGain = 1.0f;
    NoiseParams heightNoise{NoiseType::Fbm, 6.0f, 1.0f, 5, 0.0f};

    // このレイヤーが使うマテリアル（PBR のマップ一式）。
    // kNoMaterialAsset なら上の定数値だけで塗る。
    MaterialAssetId material = kNoMaterialAsset;

    // このレイヤーの UV スケール。
    float uvScale = 1.0f;
};

}  // namespace tg::compositor
