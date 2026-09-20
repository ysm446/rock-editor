#pragma once

#include "compositor/MaterialStack.h"
#include "compositor/BoundaryMaterial.h"
#include <array>
#include <optional>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

// MaterialLibrary.h は Windows ヘッダを引き込むため、ここでは合成モードの列挙だけ前方宣言する。
namespace tg::compositor { enum class BlendMode : uint32_t; }

namespace tg::renderer {

// CPU 側の生成結果。座標は右手系 Y-up、メートル。GPU の所有権を持たない。
struct MeshVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 tangent;
    DirectX::XMFLOAT2 uv;
    // 路面上の位置の道路 UV。白線などの部品が路面と同じハイトを読んで追従するために持つ。
    // 道路面自身は uv と同じ。使わないメッシュは uv を写す。
    DirectX::XMFLOAT2 roadUv{};
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

struct MaterialSettings {
    DirectX::XMFLOAT3 baseColor = {0.18f, 0.18f, 0.18f};
    float roughness = 0.35f;
    float metallic = 0.0f;
};

struct SceneMesh {
    struct Boundary {
        compositor::BoundaryMaterial material;
        float center = 0, acrossSign = 1;
    };
    MeshData geometry;
    // P0 の材質評価専用エントリ。形状を持たず、既存の評価器とGPU寿命管理を共有する。
    bool materialOnly = false;
    std::array<int, 3> connectionSources{-1, -1, -1};
    std::array<DirectX::XMFLOAT2, 3> connectionOrigins{};
    // 共通幅座標から各素材座標と描画面の接線へ戻す向き。
    std::array<float, 3> connectionAcrossSigns{1, 1, 1};
    float connectionFrameSign = 1;
    // 左右同時接続で追加する右沿道の2構成。通常は未使用。
    std::array<int, 2> connectionExtraSources{-1, -1};
    std::array<DirectX::XMFLOAT2, 2> connectionExtraOrigins{};
    std::array<float, 2> connectionExtraSigns{1, 1};
    DirectX::XMFLOAT2 connectionSecondHeightFade{};
    // 道路の区間混合。第1構成はconnectionSources[0]、残り2構成を追加する。
    std::array<int, 2> connectionRoadSources{-1, -1};
    int connectionRoadMixSource = -1;
    // 接続すべき二辺の頂点。位置の一致から逆算せず、生成時の隣接関係を記録する。
    std::vector<std::array<uint32_t, 4>> connectionSeams;
    MaterialSettings material;
    // 道路の表示用メタデータ。0は道路以外。生成時に再構築する。
    float roadMetersPerUv = 0.0f;
    // 材質のハイトで法線方向へ押し出す量（m）。0 なら形は変えない。材質が無ければ効かない。
    float displacementMeters = 0.0f;
    // P0: 直線の共通面を世界 Y 方向へ変位する。法線が分かれる縁石でも位置を揃える。
    bool connectionPrototype = false;
    // 横接続で変位を基準高さへ戻す共通境界と幅（m）。幅0は無効。
    DirectX::XMFLOAT2 connectionHeightFade{};
    std::array<float, 4> layerDisplacementMeters{0.0f, 0.0f, 0.0f, 0.0f};
    // 押し出しに使うハイトを別のメッシュ（道路面）の材質から読む。-1 なら自分の材質。
    // 白線はこれで道路面と同じ量だけ押し出され、変位後の路面に貼り付く。
    int displacementSource = -1;
    // 下地の変位に、自身の材質のハイト（黒=0）を加える。
    float additiveHeightMeters = 0.0f;
    // 路面と帯の分割差を吸収する実寸の深度補正。形状と陰影位置は動かさない。
    float surfaceDepthBiasMeters = 0.0f;
    bool showWireframe = false;
    // 接続から導出した材質。GPU参照や保存対象ではない。
    std::optional<compositor::MaterialStack> materialStack;
    // 道路のレイヤー。スロット 2〜4 の材質と、それらの被覆率を持つ道路空間マスク（RGBA8）。
    // 描画時にハイトでブレンドし、変位もブレンド後のハイトで行う。設計は docs/design/road-material-layers.md。
    std::array<std::optional<compositor::MaterialStack>, 3> layerStacks;
    struct RoadMaskPixels {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
        bool IsValid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4; }
    } roadMask, boundaryControl;
    // スロットごとのテクスチャ座標（真ならワールド XZ）と UV 反復長（m）。[0] は roadMetersPerUv と同じ。
    std::array<bool, 4> layerWorldUv{false, false, false, false};
    std::array<float, 4> layerUvRepeat{1.0f, 1.0f, 1.0f, 1.0f};
    float layerBlendRange = 0.2f;
    // 下地のハイトで絞る。0 = 使わない、1 = 高い所、2 = 低い所。しきい値と柔らかさはハイト 0〜1 の単位。
    std::array<uint32_t, 4> layerHeightGate{0u, 0u, 0u, 0u};
    std::array<float, 4> layerHeightGateThreshold{0.5f, 0.5f, 0.5f, 0.5f};
    std::array<float, 4> layerHeightGateSoftness{0.2f, 0.2f, 0.2f, 0.2f};
    // 混ぜ方。0 = マスクどおり、1 = ハイトで競合。
    std::array<uint32_t, 4> layerBlendMode{0u, 0u, 0u, 0u};
    // 道路の幅・長さ（m）と、道路 UV の向き。マスクの座標と変位の共有に使う。
    float roadWidthMeters = 0.0f;
    float roadLengthMeters = 0.0f;
    bool roadUvAlongU = false;
    // 合成モードを決める材質（一番上のレイヤーの材質）。道路面では使わない。
    compositor::MaterialAssetId blendMaterial = compositor::kNoMaterialAsset;
    // 材質の合成モード（マスク抜き / 半透明）を使うか。白線などの帯だけ真。
    bool useBlendMode = false;
    std::array<Boundary, 16> boundaries;
};

struct MeshScene {
    std::vector<SceneMesh> meshes;
};

// 不正なインデックス、非有限値、縮退した接空間は GPU へ渡さない。
bool ValidateMeshScene(const MeshScene& scene);
// 原点中心の包囲球。カメラとシャドウの既存規約に合わせる。
float MeshSceneRadius(const MeshScene& scene);
// メッシュの外周（隣接する三角形が 1 つしかない辺）を LINELIST のインデックス列で返す。
// 位置が一致する頂点は同じ点として扱うので、UV や法線の継ぎ目は外周にならない。
// ホバー / 選択のシルエット枠に使う。
std::vector<uint32_t> MeshOutlineEdges(const MeshData& data);

}  // namespace tg::renderer
