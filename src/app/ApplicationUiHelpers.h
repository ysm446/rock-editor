#pragma once

// Application の分割された実装ファイル（Application*.cpp）で共有する
// UI ヘルパと定数。Application の実装専用で、他のモジュールからは使わない。
//
// もとは Application.cpp の匿名名前空間にあったもの。複数の翻訳単位から
// 使うため、関数と変数は inline にしてある。

#include "compositor/MaterialLayer.h"
#include "core/PathUtf8.h"
#include "compositor/MaterialLibrary.h"
#include "compositor/TextureLibrary.h"
#include "renderer/Camera.h"
#include "renderer/PreviewRenderer.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

namespace rock {

inline float RadiansToDegrees(float radians) {
    return radians * (180.0f / 3.14159265358979323846f);
}

inline float DegreesToRadians(float degrees) {
    return degrees * (3.14159265358979323846f / 180.0f);
}

// -pi .. pi へ折り返す。方位角を一周させるときに使う（UI のスライダーもこの範囲）。
inline float WrapAngle(float radians) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;
    radians = std::fmod(radians + kPi, kTwoPi);
    if (radians < 0.0f) {
        radians += kTwoPi;
    }
    return radians - kPi;
}



// 既定値マーカーが参照する値。数値リテラルではなく設定構造体の初期値を使う。
inline const compositor::MaterialLayer kDefaultLayer;

inline const renderer::LightSettings kDefaultLight;
inline const renderer::ExposureSettings kDefaultExposure;
inline const renderer::CameraState kDefaultCamera;
inline const renderer::SkySettings kDefaultSky;

inline const char* const kChannelLabels[] = {"BaseColor", "Normal", "Surface", "Height"};

// ビューポートの表示モード。renderer::DebugView と並びを合わせること。
inline const char* const kDebugViewLabels[] = {
    "シェーディング",     "ベースカラー",     "法線（カメラ）", "法線（ワールド）",
    "ラフネス",           "メタルネス",       "AO",             "ハイト",
    "ハイト（ローカル）", "ワイヤーフレーム", "クレイ",
};
inline const char* const kResolutionLabels[] = {"512", "1024", "2048", "4096"};
inline constexpr uint32_t kResolutionValues[] = {512, 1024, 2048, 4096};

// プレビューの窓（マテリアル / テクスチャ / 天球）の、上の区画に使う正方形の一辺。
//
// **幅に合わせる。** 窓を広げれば絵も大きくなり、区画に余白が残らない。
// ただし窓が横長のときに幅ぶんの高さを取ると下のプロパティが消えるので、
// 残りの高さ（プロパティ 1 画面ぶんを残した値）で頭打ちにする。
// **この関数は上の区画を置く直前に呼ぶこと**（`GetContentRegionAvail` を見るため）。
inline float PreviewPaneSize() {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float maxHeight = available.y - ui::Scaled(140.0f);
    return std::max(ui::Scaled(120.0f), std::min(available.x, maxHeight));
}

// レイヤー一覧のドラッグ＆ドロップで使うペイロードの種別。
inline constexpr const char* kLayerDragDropType = "ROCK_LAYER";
// レイヤー一覧の行に並べるサムネイルの一辺（96 DPI 基準）。行の高さはこれで決まる。
// 中身（マテリアルとマスク）を読めることを優先して、文字より大きく取る。
inline constexpr float kLayerRowThumbnail = 40.0f;
// レイヤー一覧の行で、部品どうしと行の左右に空ける間隔（96 DPI 基準）。
// ImGui の ItemInnerSpacing（6）では目・サムネイル・マスク・名前が詰まって
// 1 つの塊に見える。**どれも意味の違う情報なので、読み分けられる間隔を取る。**
inline constexpr float kLayerRowGap = 12.0f;
// 目のアイコンの一辺。**サムネイルより小さくする。**
// 同じ大きさだと切り替えのアイコンが素材と同じ重みで並び、目線が散る。
inline constexpr float kLayerRowEye = 20.0f;
// レイヤーパネルの一覧側（上の区画）の高さの下限と上限（96 DPI 基準）。
// 既定値は AppSettings が持ち、境界のドラッグで変わる。
// 下限はツールバーの 1 行 + 行 2 つ + ヒントの 1 行が入る高さ。
inline constexpr float kLayerListMinHeight = 120.0f;
inline constexpr float kLayerListMaxHeight = 640.0f;

// テクスチャの拡大プレビューの一辺（96 DPI 基準）。
// サムネイル（72）では中身を確かめられないので、その 3 倍弱を取る。
inline constexpr float kTexturePreviewSize = 200.0f;

// テクスチャ一覧からマップ欄へのドラッグ＆ドロップで使うペイロードの種別。
inline constexpr const char* kTextureDragDropType = "ROCK_TEXTURE";
// マテリアル一覧から Surface のマテリアル欄（プロパティの行 / ノードのサムネイル）へ
// ドラッグ＆ドロップで割り当てるときのペイロードの種別。中身は MaterialAssetId。
inline constexpr const char* kMaterialDragDropType = "ROCK_MATERIAL";
// アセットの帯のレイヤーマテリアル / 境界マテリアルのサムネイルをドラッグしたときのペイロード
//（どちらも graph::SurfaceId）。Road の沿道欄の該当行へ落とすと、その区間に割り当たる。
inline constexpr const char* kLayerMaterialDragDropType = "ROCK_LAYER_MATERIAL";
inline constexpr const char* kBoundaryMaterialDragDropType = "ROCK_BOUNDARY_MATERIAL";
// アセットの帯で、ライブラリの ID を持たないファイル（未読み込みの画像・シーンなど）をドラッグしたときのペイロード。
// 中身はパスの wchar_t 文字列（終端込み）。一覧のフォルダ・左のフォルダ階層へ落とすと移動する。
inline constexpr const char* kAssetPathDragDropType = "ROCK_ASSET_PATH";

// 直前の ui::PropertyCombo の値の矩形（サムネイル＋コンボ）を受け口にして、type のペイロードを受ける。
// 落とされた ID を outId に入れて真。コンボ本体だけでなくサムネイルにも落とせるようにするための部品。
template <typename Id>
inline bool AcceptComboDrop(const char* type, Id& outId) {
    ImVec2 min, max;
    ui::LastPropertyComboRect(min, max);
    bool accepted = false;
    if (ImGui::BeginDragDropTargetCustom(ImRect(min, max), ImGui::GetID(type))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type); payload != nullptr) {
            outId = *static_cast<const Id*>(payload->Data);
            accepted = true;
        }
        ImGui::EndDragDropTarget();
    }
    return accepted;
}
inline constexpr const char* kTextureRemoveModalTitle = "テクスチャを削除";

// テクスチャの一覧に出すフォーマット名。DXGI の名前は長いので短く言い換える。
inline const char* TextureFormatLabel(const compositor::LibraryTexture& entry) {
    return entry.isFloat ? "RGBA16F (リニア)" : "RGBA8 (sRGB / リニア)";
}

// ステータスバーの通知を残す時間（秒）。情報だけが時間で消える。
inline constexpr float kStatusHoldSeconds = 6.0f;

// ビューポートの背景色の既定値。Application のメンバ初期化と揃えること。
inline constexpr float kDefaultClearColor[3] = {0.09f, 0.09f, 0.11f};

// 解像度コンボの選択位置。一致するものが無ければ 1（1024）に寄せる。
inline int ResolutionIndex(uint32_t resolution) {
    for (int i = 0; i < IM_ARRAYSIZE(kResolutionValues); ++i) {
        if (kResolutionValues[i] == resolution) {
            return i;
        }
    }
    return 1;
}

// 直前のアイテムへマテリアル一覧からのドラッグを受ける。落とせば slot を差し替えて真。
// ID の無いアイテム（画像や Dummy）でも矩形から受け口を作れる。
inline bool AcceptMaterialDrop(compositor::MaterialAssetId& slot) {
    bool changed = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialDragDropType);
            payload != nullptr) {
            slot = *static_cast<const compositor::MaterialAssetId*>(payload->Data);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

// マテリアルを選ぶ行。サムネイル付きの一覧から選ぶ。
inline bool DrawMaterialSlotRow(const char* label, compositor::MaterialAssetId& slot,
                         const compositor::MaterialLibrary& library, bool showThumbnail = false,
                         const char* hint = "「なし」ならレイヤーの定数値だけで塗る") {
    ui::PropertyLabel(label, hint);

    std::string preview = "なし";
    if (const compositor::MaterialAsset* current = library.Find(slot); current != nullptr) {
        preview = current->name;
    }

    const float thumbnailSize = showThumbnail ? ui::Scaled(kLayerRowThumbnail) : ImGui::GetFrameHeight();
    bool changed = false;
    if (showThumbnail) {
        const float rowY = ImGui::GetCursorPosY();
        ui::ThumbnailImage(static_cast<ImTextureID>(library.ThumbnailHandle(slot).ptr), thumbnailSize);
        // サムネイルもコンボと同じ受け口。狙う先が広いほど落としやすい。
        changed |= AcceptMaterialDrop(slot);
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowY + (thumbnailSize - ImGui::GetFrameHeight()) * 0.5f);
    }
    ImGui::SetNextItemWidth(
        std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        if (ImGui::Selectable("なし", slot == compositor::kNoMaterialAsset)) {
            slot = compositor::kNoMaterialAsset;
            changed = true;
        }
        for (const compositor::MaterialAsset& asset : library.Entries()) {
            ImGui::PushID(static_cast<int>(asset.id));
            if (showThumbnail) {
                const auto thumbnail = ui::ThumbnailButton("##materialThumbnail",
                    static_cast<ImTextureID>(library.ThumbnailHandle(asset.id).ptr), thumbnailSize, slot == asset.id);
                if (thumbnail.clicked) {
                    slot = asset.id;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
            } else if (asset.thumbnail.IsValid()) {
                ImGui::Image(static_cast<ImTextureID>(asset.thumbnail.srv.gpu.ptr),
                             ImVec2(thumbnailSize, thumbnailSize));
                ImGui::SameLine();
            }
            if (ImGui::Selectable(asset.name.c_str(), slot == asset.id)) {
                slot = asset.id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // マテリアル一覧からドラッグしてきたものを受ける。テクスチャのコンボと同じ作りで、
    // ドラッグ中にコンボは開けないので、直前のアイテムは必ずコンボ本体になる。
    changed |= AcceptMaterialDrop(slot);
    ui::PropertyEnd();
    return changed;
}

// 旧Surfaceの定数設定を保持し、新しく選び直した場合は素材の標準設定に戻す。
inline bool DrawMeshMaterialSlotRow(const char* label, std::optional<compositor::MaterialLayer>& slot,
                                    const compositor::MaterialLibrary& library) {
    auto material = slot ? slot->material : compositor::kNoMaterialAsset;
    if (!DrawMaterialSlotRow(label, material, library, true,
        "「なし」を選ぶとノードの既定色に戻る。マテリアル一覧からドラッグして割り当てもできる")) return false;
    slot.reset();
    if (material != compositor::kNoMaterialAsset) {
        slot.emplace();
        slot->material = material;
    }
    return true;
}

// テクスチャを選ぶコンボ。行の中に置く部品。
inline bool DrawTextureCombo(const char* id, compositor::TextureId& slot,
                      const compositor::TextureLibrary& library, float width) {
    std::string preview = "なし";
    bool missing = false;
    if (const compositor::LibraryTexture* current = library.Find(slot); current != nullptr) {
        preview = current->name;
        missing = current->missing;
    }

    bool changed = false;
    ImGui::SetNextItemWidth(width);
    // リンク切れの画像を指しているときは、名前を警告色で出す。
    // 割り当ては保ってあるので「なし」とは違い、繋ぎ直せば戻る。
    if (missing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
    }
    const bool open = ImGui::BeginCombo(id, preview.c_str());
    if (missing) {
        ImGui::PopStyleColor();
    }
    if (open) {
        if (ImGui::Selectable("なし", slot == compositor::kNoTexture)) {
            slot = compositor::kNoTexture;
            changed = true;
        }
        for (const compositor::LibraryTexture& entry : library.Entries()) {
            ImGui::PushID(static_cast<int>(entry.id));
            if (entry.missing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
            }
            if (ImGui::Selectable(entry.name.c_str(), slot == entry.id)) {
                slot = entry.id;
                changed = true;
            }
            if (entry.missing) {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // テクスチャ一覧からドラッグしてきた画像を受ける。
    // ドラッグ中にコンボは開けないので、直前のアイテムは必ずコンボ本体になる。
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTextureDragDropType);
            payload != nullptr) {
            slot = *static_cast<const compositor::TextureId*>(payload->Data);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }

    // 割り当ててあるものをホバーで出す。コンボの幅では名前が入りきらず、
    // `T_Dusty_Gravel_Grou...` のように切れて見分けられないため。
    //
    // **ドロップの受け口より後に置くこと。** SetTooltip は内部でウィンドウを作るので、
    // 先に呼ぶと BeginDragDropTarget が見る「直前のアイテム」が変わってしまう。
    // ドラッグ中は ImGui 自身がプレビューを出すので、重ねない。
    // 遅延はプロパティ行のツールチップと揃える（即座には出さない）。
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
        ImGui::GetDragDropPayload() == nullptr) {
        if (const compositor::LibraryTexture* current = library.Find(slot); current != nullptr) {
            if (current->missing) {
                ImGui::SetTooltip("%s\nリンク切れ: ファイルが見つかりません。"
                                  "テクスチャ一覧の右クリックから繋ぎ直す",
                                  current->name.c_str());
            } else {
                ImGui::SetTooltip("%s\n%u x %u", current->name.c_str(), current->texture.width,
                                  current->texture.height);
            }
        } else {
            ImGui::SetTooltip("なし\nテクスチャ一覧からドラッグしても割り当てられる");
        }
    }
    return changed;
}

// マップをどの UV で読むかのコンボ（テクスチャの行の右端）。uvSets が無ければ何もしない。
// UV を 2 つ持つのはモデルだけで、道路の合成とマテリアルの球では 1 つ目の UV で読む。
inline bool DrawUvSetCombo(uint32_t* uvSets, compositor::MaterialMap map, float width) {
    if (uvSets == nullptr) return false;
    static const char* const kUvSetLabels[] = {"UV1", "UV2"};
    const uint32_t bit = compositor::MaterialMapBit(map);
    int uvSet = (*uvSets & bit) != 0 ? 1 : 0;
    ImGui::SetNextItemWidth(width);
    if (!ImGui::Combo("##uvSet", &uvSet, kUvSetLabels, IM_ARRAYSIZE(kUvSetLabels))) return false;
    *uvSets = uvSet != 0 ? (*uvSets | bit) : (*uvSets & ~bit);
    return true;
}

inline constexpr float kUvSetComboWidth = 60.0f;

// テクスチャスロットを選ぶ行。RGB をそのまま使うマップ（ベースカラー / 法線）用。
// uvSets を渡すと、テクスチャがあるときに右へ UV の選択を出す（map はそのビット）。
inline bool DrawTextureSlotRow(const char* label, compositor::TextureId& slot,
                        const compositor::TextureLibrary& library, uint32_t* uvSets = nullptr,
                        compositor::MaterialMap map = compositor::MaterialMap::BaseColor) {
    ui::PropertyLabel(label, uvSets ? "「なし」なら定数値を使う。右は読む UV（UV2 はモデルの 2 つ目の UV）"
                                    : "「なし」なら定数値を使う");
    const bool showUv = uvSets != nullptr && slot != compositor::kNoTexture;
    const float uvWidth = showUv ? ui::Scaled(kUvSetComboWidth) : 0.0f;
    const float spacing = showUv ? ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
    // テクスチャの欄は UV の無い行と同じ幅に保ち、UV のコンボのぶんだけ行を右へ伸ばす（窓が狭いときだけ縮める）。
    const float width = std::max(ui::Scaled(60.0f),
        std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x - uvWidth - spacing));
    bool changed = DrawTextureCombo("##value", slot, library, width);
    if (showUv) {
        ImGui::SameLine(0.0f, spacing);
        changed |= DrawUvSetCombo(uvSets, map, uvWidth);
    }
    ui::PropertyEnd();
    return changed;
}

// スカラーのマップを選ぶ行。テクスチャに加えて、どのチャンネルを読むかも選ぶ。
// Megascans の _ORD のように 1 枚へ複数のマップを詰めたテクスチャがあるため。
// uvSets を渡すと、チャンネルの右へ UV の選択も出す（map はそのビット）。
inline bool DrawMapSlotRow(const char* label, compositor::MapSlot& slot,
                    const compositor::TextureLibrary& library, uint32_t* uvSets = nullptr,
                    compositor::MaterialMap map = compositor::MaterialMap::Roughness) {
    ui::PropertyLabel(label, uvSets ? "「なし」なら定数値を使う。右は読むチャンネルと UV（UV2 はモデルの 2 つ目の UV）"
                                    : "「なし」なら定数値を使う。右は読むチャンネル");

    const bool hasTexture = slot.texture != compositor::kNoTexture;
    const bool showUv = uvSets != nullptr && hasTexture;
    const float channelWidth = ui::Scaled(52.0f);
    const float uvWidth = showUv ? ui::Scaled(kUvSetComboWidth) : 0.0f;
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    // UV のコンボはテクスチャの欄を削らず、そのぶん行を右へ伸ばす（窓が狭いときだけ縮める）。
    const float uvExtra = showUv ? uvWidth + spacing : 0.0f;
    const float comboWidth =
        std::max(ui::Scaled(60.0f), std::min(ui::Scaled(ui::kComboMaxWidth), available - uvExtra) - channelWidth - spacing);

    bool changed = DrawTextureCombo("##texture", slot.texture, library, comboWidth);

    if (slot.texture != compositor::kNoTexture) {
        ImGui::SameLine(0.0f, spacing);
        static const char* const kTextureChannelLabels[] = {"R", "G", "B", "A"};
        int channel = static_cast<int>(slot.channel);
        ImGui::SetNextItemWidth(channelWidth);
        if (ImGui::Combo("##channel", &channel, kTextureChannelLabels,
                         IM_ARRAYSIZE(kTextureChannelLabels))) {
            slot.channel = static_cast<compositor::TextureChannel>(channel);
            changed = true;
        }
        if (showUv) {
            ImGui::SameLine(0.0f, spacing);
            changed |= DrawUvSetCombo(uvSets, map, uvWidth);
        }
    }

    ui::PropertyEnd();
    return changed;
}

// ライトのギズモが残る時間（秒）。掴むのをやめてから薄くなって消える。
inline constexpr double kLightGizmoFadeSeconds = 0.35;
// ライトを掴んだときの感度。参考にした terrain-editor と同じ 0.25 度 / ピクセル。
inline constexpr float kLightDegreesPerPixel = 0.25f;

// ビューポートに重ねる線を描くための投影。カメラの行列をそのまま使う。
struct ProjectedPoint {
    ImVec2 screen{};
    bool visible = false;
};

inline ProjectedPoint ProjectToViewport(const DirectX::XMMATRIX& viewProjection,
                                 const DirectX::XMFLOAT3& world, const ImVec2& min,
                                 const ImVec2& size) {
    using namespace DirectX;
    const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&world), viewProjection);
    const float w = XMVectorGetW(clip);
    ProjectedPoint out;
    // カメラの後ろに回った点は描かない。
    if (w <= 1e-4f) {
        return out;
    }
    const float ndcX = XMVectorGetX(clip) / w;
    const float ndcY = XMVectorGetY(clip) / w;
    out.screen = ImVec2(min.x + (ndcX * 0.5f + 0.5f) * size.x,
                        min.y + (0.5f - ndcY * 0.5f) * size.y);
    out.visible = true;
    return out;
}

// ビューポート左下に置く座標軸ギズモ。
//
// 軸の色は DCC 共通の意味色（X=赤 / Y=緑 / Z=青）なので、テーマからは引かない。
// 透視投影は掛けず、向きだけを見せる。
inline void DrawAxisGizmo(const renderer::Camera& camera, const ImVec2& viewportMin,
                   const ImVec2& viewportMax) {
    const renderer::CameraBasis basis = camera.Basis();

    const float radius = ui::Scaled(30.0f);
    const float margin = ui::Scaled(16.0f);
    const ImVec2 center(viewportMin.x + margin + radius, viewportMax.y - margin - radius);
    if (center.x + radius > viewportMax.x || center.y - radius < viewportMin.y) {
        return;  // ビューポートが小さすぎる。
    }

    struct Axis {
        float direction[3];
        ImU32 color;
        const char* label;
    };
    static const Axis kAxes[] = {
        {{1.0f, 0.0f, 0.0f}, IM_COL32(226, 96, 96, 255), "X"},
        {{0.0f, 1.0f, 0.0f}, IM_COL32(124, 196, 104, 255), "Y"},
        {{0.0f, 0.0f, 1.0f}, IM_COL32(96, 146, 226, 255), "Z"},
    };

    struct Projected {
        ImVec2 tip;
        float depth = 0.0f;  // 正なら画面の奥を向いている
        ImU32 color = 0;
        const char* label = nullptr;
    };

    const auto project = [](const float* axis, const DirectX::XMFLOAT3& b) {
        return axis[0] * b.x + axis[1] * b.y + axis[2] * b.z;
    };

    Projected projected[IM_ARRAYSIZE(kAxes)];
    for (int i = 0; i < IM_ARRAYSIZE(kAxes); ++i) {
        const float x = project(kAxes[i].direction, basis.right);
        const float y = project(kAxes[i].direction, basis.up);
        projected[i].depth = project(kAxes[i].direction, basis.forward);
        projected[i].tip = ImVec2(center.x + x * radius, center.y - y * radius);
        projected[i].label = kAxes[i].label;

        // 奥を向いている軸は落として、手前と見分けられるようにする。
        const ImU32 color = kAxes[i].color;
        projected[i].color = (projected[i].depth > 0.0f)
                                 ? ((color & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT))
                                 : color;
    }

    // 奥の軸から先に描く。
    std::sort(std::begin(projected), std::end(projected),
              [](const Projected& a, const Projected& b) { return a.depth > b.depth; });

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // 文字を置くぶん、線を先端の手前で止める幅。
    const float labelGap = ui::Scaled(7.0f);
    for (const Projected& axis : projected) {
        const ImVec2 delta(axis.tip.x - center.x, axis.tip.y - center.y);
        const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);

        // **線は文字の手前で止める。** 先端に丸を置かないので、
        // 文字まで引くと字画と重なって読めなくなる。
        // 真正面を向いた軸は線が点に潰れるため、文字だけを置く。
        if (length > labelGap) {
            const ImVec2 direction(delta.x / length, delta.y / length);
            const ImVec2 lineEnd(axis.tip.x - direction.x * labelGap,
                                 axis.tip.y - direction.y * labelGap);
            drawList->AddLine(center, lineEnd, axis.color, ui::Scaled(1.6f));
        }

        // **文字は軸の色で描く。** 下に敷く丸が無いので、
        // 暗い色だとビューポートの背景に沈んで読めない。
        const ImVec2 textSize = ImGui::CalcTextSize(axis.label);
        const ImVec2 textPos(axis.tip.x - textSize.x * 0.5f, axis.tip.y - textSize.y * 0.5f);
        drawList->AddText(textPos, axis.color, axis.label);
    }
}


}  // namespace rock
