#pragma once
#include "geometry/Volume.h"

namespace rock::geometry {
// VolumeSurface 内部用。入力検証と最終的な閉包・体積の検証は呼び出し元で行う。
Mesh ExtractDualContour(const VolumeGrid& grid, std::string& error);
}
