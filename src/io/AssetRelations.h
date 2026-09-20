#pragma once

#include "io/ProjectWorkspace.h"

namespace tg::io {

// アセットファイルを退避（削除）する前の参照関係の検査結果。
struct AssetRelations {
    std::filesystem::path target;
    // 対象を参照している文書（シーン・共有アセット・旧プロジェクト）。
    std::vector<std::filesystem::path> referencers;
    // 対象が参照している素材と、シーンのプレビュー画像。削除はしない。
    std::vector<std::filesystem::path> related;
    // 一緒に退避するファイル（`.meta`）。
    std::vector<std::filesystem::path> companions;
    std::vector<std::string> companionVersions;
    std::filesystem::file_time_type modified{};
    uintmax_t size = 0;
    // 走査をすべて読めたか。偽なら削除を許さない。
    bool complete = false;
};

AssetRelations InspectAssetRelations(ProjectWorkspace& workspace, const std::filesystem::path& target);
// 確認時から変わっていない場合だけ、元ファイルと .meta をルート内の退避フォルダへ移す。
bool RetireAsset(ProjectWorkspace& workspace, const AssetRelations& approved);
// 同じフォルダの中で名前を変える。ファイルなら隣の .meta も新しい名前へ揃え、フォルダはそのまま改名する。
// 参照は ID で解決するので、改名後に走査し直せば切れない。ルートと目印ファイル・内部フォルダは改名しない。
// 成功したら新しいパスを返し、失敗したら空を返す（途中で失敗した分は戻す）。
std::filesystem::path RenameAsset(ProjectWorkspace& workspace, const std::filesystem::path& target,
                                  const std::string& newName);
// ファイルまたはフォルダを同じ名前のまま別のフォルダ（ルート内）へ移す。ファイルは .meta も一緒に動かし、
// フォルダは中身ごと移す（ルート・内部フォルダは動かさず、自分自身やその配下へは移さない）。
// 参照は ID で解決するので切れない。成功したら移動先のパスを返し、失敗したら空を返す。
std::filesystem::path MoveAsset(ProjectWorkspace& workspace, const std::filesystem::path& target,
                                const std::filesystem::path& directory);

}  // namespace tg::io
