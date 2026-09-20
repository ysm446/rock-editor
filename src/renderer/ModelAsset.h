#pragma once

#include "compositor/MaterialLayer.h"
#include "renderer/MeshData.h"

#include <DirectXCollision.h>
#include <DirectXMath.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// FBX から読んだ 3D モデル。terrain-graph の ModelAsset を移植したもの（配置ノードは持たない）。
// 仕様は docs/reference/model-assets.md。
namespace tg::renderer {

// マテリアルスロット 1 つぶんの、FBX のマテリアルに書かれていた情報。
// 「FBX のマテリアルから作成」でマテリアルアセットの初期値に使う。
struct ModelSlotSource {
    std::string name;
    // 拡散色のテクスチャ。FBX の相対パス → 絶対パス → FBX と同じフォルダの同名の順に探し、
    // 見つかったものだけを入れる。無ければ空。
    std::filesystem::path baseColorTexture;
    DirectX::XMFLOAT3 baseColor{1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
};

// FBX のノード 1 つ。親は nodes の添字（-1 = 一番上）で、親は必ず子より前に並ぶ。
// bindLocal は親の座標から見たノードの変換（行ベクトルの規約。world = bindLocal * 親の world）。
// 一番上のノードは右手系 Y-up・m への変換を含む。
struct ModelNode {
    std::string name;
    int parent = -1;
    DirectX::XMFLOAT4X4 bindLocal{};
};

// 部品。ノードとスロットの組ごとに 1 つ。頂点はノードの座標（ワールドへは ModelNodeWorlds の行列で運ぶ）。
struct ModelPart {
    MeshData mesh;
    uint32_t slot = 0;
    uint32_t node = 0;
    // ノードの座標での範囲（ノードを選んだときの枠）。
    DirectX::XMFLOAT3 minimum{}, maximum{};
};

struct ModelLod {
    std::vector<ModelPart> parts;
    uint32_t triangles = 0;
};

struct ModelGeometry {
    std::vector<ModelLod> lods;
    std::vector<ModelSlotSource> slots;
    std::vector<ModelNode> nodes;
    // 読んだままの姿勢（ノードを回す前）でのモデル全体の範囲。底面の中心（ModelPivotMatrix）もこれで決める。
    DirectX::XMFLOAT3 minimum{}, maximum{};
};

// ノードに足す回転（Model ノードの設定）。ノードの名前で指す（読み直しても番号に依存しない）。
struct ModelNodeRotation {
    std::string node;
    float rotationDegrees[3] = {0.0f, 0.0f, 0.0f};
};

// CPU 形状は不変・共有。履歴へ頂点配列を複製しない。
// GPU リソースを持たないので、アンドゥのスナップショットへそのまま複製できる。
struct ModelAsset {
    uint64_t id = 0;
    // 共有アセット（.tgmodel）の置き場所と永続 ID。未保存なら空。
    std::filesystem::path assetPath;
    std::string assetUid;
    std::string name;
    // 元の FBX。
    std::filesystem::path path;
    std::shared_ptr<const ModelGeometry> geometry;
    // スロットごとのマテリアル。未割り当ては kNoMaterialAsset（灰色で描く）。
    std::vector<compositor::MaterialAssetId> materials;
    // FBX の座標に掛ける倍率。単位の宣言と中身が食い違う FBX（cm と宣言して m で作ったもの）を
    // 実寸へ直すためのもの。形状（geometry）には焼き込まず、寸法の表示とシーンへの配置で掛ける。
    float scale = 1.0f;
    std::string error;
};

// モデルの底面の中心（形状の境界ボックスの X・Z の中央、Y の最小）を原点へ移し、アセットの倍率を掛ける行列
// （DirectXMath の行ベクトル規約）。これにノードの倍率・回転・位置を掛けるとワールド行列になる。形状が無ければ単位行列。
DirectX::XMMATRIX ModelPivotMatrix(const ModelAsset& model);
// 倍率 → 回転（X / Y / Z の度。Z → X → Y の順、RollPitchYaw と同じ）→ 平行移動の行列。
DirectX::XMMATRIX NodeTransformMatrix(const float position[3], const float rotationDegrees[3], float scale);
// 回転行列（倍率を含まない 3x3）を NodeTransformMatrix と同じ規約の角度（度）へ戻す。
void RotationToDegrees(DirectX::FXMMATRIX rotation, float degrees[3]);
// world を掛けたモデルのワールド空間の境界ボックス。形状が無ければ偽。
bool ModelWorldBounds(const ModelAsset& model, DirectX::FXMMATRIX world, DirectX::BoundingBox& bounds);

// 各ノードのモデル座標での行列（geometry->nodes と同じ並び）。rotations にあるノードは、ノードの原点を中心に
// X / Y / Z の度（NodeTransformMatrix と同じ順）を回す。軸はモデルの軸（読んだままの姿勢で見た X / Y / Z。
// Y が上）で、親を回せば子の軸も一緒に回る。
void ModelNodeWorlds(const ModelGeometry& geometry, const std::vector<ModelNodeRotation>& rotations,
                     std::vector<DirectX::XMFLOAT4X4>& worlds);

// FBX を読み、右手系 Y-up・メートルへ変換する。失敗したら asset.error に理由を入れて偽を返す
// （geometry と materials は変えない）。
bool LoadModel(const std::filesystem::path& path, ModelAsset& asset);

}  // namespace tg::renderer
