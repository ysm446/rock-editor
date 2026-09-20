#pragma once
#include "graph/SurfaceLayout.h"
#include <nlohmann/json_fwd.hpp>

namespace tg::io {
nlohmann::json WriteSurfaceLayouts(const graph::SurfaceLayoutDocument& document);
// 失敗時は出力を変更しない。GPUや現在のプロジェクトを書き換える前に呼ぶ。
bool ReadSurfaceLayouts(const nlohmann::json& value, graph::SurfaceLayoutDocument& document, std::string& error);
}  // namespace tg::io
