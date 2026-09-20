#pragma once

#include "compositor/MaterialLayer.h"
#include "compositor/TextureLibrary.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tg::compositor {

// マテリアル 1 つぶん。PBR のマップ一式に名前を付けたもの。
//
// レイヤーはマテリアルを 1 つ参照する（Quixel Mixer と同じ形）。
// マップを個別に差し替えるのではなく、マテリアルを差し替えることで見た目を変える。
// マスクだけはレイヤー固有なので、ここには入れない。
// 不透明度の扱い。材質の属性として持ち、白線などの帯メッシュを描くときに効く。
// 道路面のような下に何も無いメッシュでは常に不透明として描く。
enum class BlendMode : uint32_t {
    Opaque = 0,       // 不透明度を無視する
    Masked = 1,       // しきい値未満をくり抜く。深度と影はそのまま
    Translucent = 2,  // 不透明度でそのまま合成する。影は落とさない
};

// マテリアルのマップ。MaterialAsset::mapUvSets のビットの位置で、シェーダの TG_MAP_* と一致させること。
enum class MaterialMap : uint32_t {
    BaseColor = 0,
    Normal = 1,
    Roughness = 2,
    Metallic = 3,
    AmbientOcclusion = 4,
    Height = 5,
    Opacity = 6,
    Count = 7,
};

struct MaterialAsset {
    MaterialAssetId id = kNoMaterialAsset;
    // 共有アセットの置き場所と永続 ID（`.tgmat`）。未保存なら空。
    // 実行中の id とは別物。id は GPU 用の通し番号で、ファイルには書かない。
    std::filesystem::path assetPath;
    std::string assetUid;
    std::string name;

    // 未指定のスロットは下の定数を使う。
    // ベースカラーと法線は RGB をそのまま使うのでチャンネル指定は要らない。
    TextureId baseColor = kNoTexture;  // sRGB として読む
    TextureId normal = kNoTexture;     // タンジェント空間法線（リニア）
    // スカラーのマップは「テクスチャ + チャンネル」で指定する。
    // Megascans の _ORD のように 1 枚へ詰めたテクスチャを使えるようにするため。
    MapSlot roughness;
    MapSlot metallic;
    MapSlot ambientOcclusion;
    MapSlot height;
    // 不透明度。マップが無ければ opacityValue。合成結果の Surface の A に入る。
    MapSlot opacity;
    float opacityValue = 1.0f;
    BlendMode blendMode = BlendMode::Opaque;
    float maskThreshold = 0.5f;

    // **乗算の中立値なので 1.0。** マップがあるときは掛け算で効く
    // （`layerBaseColor *= テクスチャ`）ため、1 以外を既定にすると
    // 外から持ち込んだ素材のアルベドが黙って変わってしまう。
    // マップが無いスロットでは、この値がそのままベースカラーになる。
    DirectX::XMFLOAT3 baseColorTint = {1.0f, 1.0f, 1.0f};

    // **ベースカラーだけの調整。** ティントを掛けた**あと**に効く。
    //
    // ティント（掛ける色）では彩度を上げられず、色相も回せない。
    // スキャン素材を混ぜるときの「少し彩度を落として馴染ませる」「もう少し黄色へ」
    // 「暗すぎる素材を持ち上げる」をここで行う。
    //
    // 計算はリニア空間のまま（`AdjustBaseColor`）。色相は灰色の軸まわりの回転、
    // 彩度は輝度へ寄せる / 離す、明るさは最後に掛けて 0〜1 へクランプする。
    // 法線やラフネスには掛けない（意味を持たないため）。
    float hueShiftDegrees = 0.0f;  // -180〜180
    float saturation = 1.0f;       // 0 で無彩色、1 でそのまま
    float brightness = 1.0f;       // ベースカラーに掛ける倍率。1 でそのまま。結果は 0〜1 に収める

    float roughnessValue = 0.5f;
    float metallicValue = 0.0f;
    float ambientOcclusionValue = 1.0f;

    // **法線マップの緑を反転して読むか。**
    //
    // 法線マップには 2 つの規約がある。
    //   OpenGL 規約 : 緑 = 画像の上向き（−V）。Megascans などの既定
    //   DirectX 規約: 緑 = 画像の下向き（+V）
    // このアプリが自分で作る法線（ハイトの勾配）と、平面の接空間は
    // **DirectX 規約**に揃えてあるので、OpenGL 規約のマップはそのままだと
    // V 方向（平面では世界の Z）の陰影が反転する。
    //
    // **既定は反転する（＝ OpenGL 規約として読む）。** 手元の素材（Megascans）が
    // そちらで、既定を DirectX にすると読み込んだ素材が軒並み反転して見えるため。
    bool flipNormalGreen = true;

    // **マップごとにどの UV で読むか。** MaterialMap の位置のビットが立っていれば 2 つ目の UV、無ければ 1 つ目。
    // ライトマップ（AO）だけを 2 つ目の UV に展開した FBX（XNA の戦車など）のため。
    // UV を 2 つ持つのはモデルだけで、道路の合成とマテリアルの球は 1 つ目の UV で読む。
    uint32_t mapUvSets = 0;

    // 一覧に出すサムネイル。マップかパラメータを変えたら作り直す。
    rhi::GpuTexture thumbnail;
    bool thumbnailDirty = true;
};

// チャンネル指定をシェーダへ渡す形へ詰める。並びは TG_CHANNEL_SLOT_* と一致させること。
uint32_t PackMaterialChannels(const MaterialAsset& asset);

inline constexpr uint32_t MaterialMapBit(MaterialMap map) { return 1u << static_cast<uint32_t>(map); }
// そのマップを 2 つ目の UV で読むか。
inline bool UsesSecondUv(const MaterialAsset& asset, MaterialMap map) {
    return (asset.mapUvSets & MaterialMapBit(map)) != 0;
}

// マテリアルを保持し、サムネイルを作る。
class MaterialLibrary {
public:
    void Destroy(rhi::Device& device);

    MaterialAssetId Add(const std::string& name);
    // ID を保ったまま作り直す。**アンドゥで削除を取り消すときに使う。**
    // Add で作ると新しい ID が振られ、レイヤーからの参照が切れてしまう。
    MaterialAsset& RestoreAsset(MaterialAssetId id, const std::string& name);
    MaterialAssetId Duplicate(const MaterialAsset& source);
    void Remove(rhi::Device& device, MaterialAssetId id);
    void Clear(rhi::Device& device);

    const std::vector<MaterialAsset>& Entries() const { return m_entries; }
    const MaterialAsset* Find(MaterialAssetId id) const;
    MaterialAsset* FindMutable(MaterialAssetId id);

    // サムネイルの作り直しを予約する。マップやパラメータを変えたら呼ぶ。
    void MarkThumbnailDirty(MaterialAssetId id);

    // 1 枚の ORD テクスチャを AO(R) / ラフネス(G) / ハイト(B) へまとめて割り当てる。
    // Megascans の `_ORD` の並びに合わせてある。
    void AssignOrdTexture(MaterialAssetId id, TextureId texture);

    // 予約されたサムネイルを作る。GPU 待機を伴うため、フレームの外で呼ぶ。
    void ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                            const TextureLibrary& textures);

    // 一覧で使うサムネイルのハンドル。まだ無ければ ptr が 0。
    D3D12_GPU_DESCRIPTOR_HANDLE ThumbnailHandle(MaterialAssetId id) const;

private:
    bool BuildThumbnail(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                        const TextureLibrary& textures, MaterialAsset& asset);

    std::vector<MaterialAsset> m_entries;
    MaterialAssetId m_nextId = 1;
};

}  // namespace tg::compositor
