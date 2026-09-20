#pragma once

#include <filesystem>

namespace rock {

// エクスプローラでファイルの場所を開き、そのファイルを選択状態にする。
// 保存したものをすぐ確認できるようにするために使う。
void RevealFileInExplorer(const std::filesystem::path& path);

}  // namespace rock
