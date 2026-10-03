#pragma once
#include "geometry/ShapeMask.h"
#include "geometry/Terrain.h"

#include <functional>
#include <stop_token>
#include <string>

namespace rock::geometry {
// 山グラフの地形（ハイトの格子）に対する侵食と変形。格子は 0〜1 の高さで、HeightmapSettings の寸法で m に直して計算する。
//
// Terrain Erode: 熱侵食（安息角より急な斜面の土が下へ崩れ、裾に溜まる。崖錐の斜面）と、水侵食（流れの集まる筋を
// 溝に彫る。雨の流路）。どちらも格子の上で計算し、GPU は使わない。
// Terrain Deform: UV のマスク（Rock Scatter の Coverage など）で地形を盛る・えぐる（岩の根元の土）。

struct TerrainErodeSettings {
    // --- 熱侵食（崩れ） ---
    float talusAngle = 35.0f;    // 安息角（度、10〜80）。これより急な斜面の土が低い隣へ崩れる
    int thermalIterations = 40;  // 回（0〜500）。0 で崩さない
    float thermalRate = 0.5f;    // 1 回に崩す割合（0〜1）
    // --- 水侵食（流路） ---
    float rillDepth = 0.4f;      // 最も流れの集まる筋を彫る深さ（m、0〜50）。0 で彫らない
    float rillSharpness = 0.6f;  // 流れの量（0〜1）^ この値（0.1〜4）。小さいほど細い筋も彫れる
    float rillWidth = 1.0f;      // 筋をぼかす幅（m、0〜50）。0 でぼかさない
    // 流路の反復（1〜50）。彫る → 流れを求め直す、を繰り返す。溝が深くなるほど流れが集まり、見える溝になる。
    // 深さは反復で分けて彫る（合計はおよそ rillDepth）。1 で従来どおり 1 回。
    int rillIterations = 1;
    bool operator==(const TerrainErodeSettings&) const = default;
};

struct TerrainDeformSettings {
    float amount = -0.3f;  // マスクの白い所を動かす量（m、-20〜20）。正で盛る、負でえぐる
    float blur = 0.5f;     // マスクの縁をぼかす幅（m、0〜50）
    bool operator==(const TerrainDeformSettings&) const = default;
};

bool ValidateTerrainErodeSettings(const TerrainErodeSettings& settings, std::string& error);
bool ValidateTerrainDeformSettings(const TerrainDeformSettings& settings, std::string& error);

// 侵食した格子を返す（寸法は入力と同じ）。失敗・取消では空の格子を返し、error に理由を入れる。
HeightGrid ErodeTerrain(const HeightGrid& grid, const HeightmapSettings& terrain, const TerrainErodeSettings& settings,
                        std::string& error, std::stop_token stop = {}, const std::function<void(int)>& progress = {});
// マスク（地形の UV = 格子の u, v）で盛る・えぐった格子を返す。0〜1 の外へ出た高さは切り詰める（最高・最低の外には出ない）。
HeightGrid DeformTerrain(const HeightGrid& grid, const HeightmapSettings& terrain, const MaskImage& mask, bool invertMask,
                         const TerrainDeformSettings& settings, std::string& error);
}  // namespace rock::geometry
