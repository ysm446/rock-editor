#pragma once

#include "rhi/Common.h"

#include <directx-dxc/dxcapi.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace tg::rhi {

// DXC を使った HLSL のランタイムコンパイラ。
// シェーダはソースツリーの shaders/ を直接参照し、更新を検出して再コンパイルできる。
class ShaderCompiler {
public:
    ShaderCompiler() = default;
    ~ShaderCompiler();

    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    bool Create(const std::filesystem::path& shaderRoot);
    void Destroy();

    // コンパイル結果（DXIL）をディスクへ残す場所。設定すると、同じソース・引数の
    // コンパイルは 2 回目以降ファイルを読むだけになる（起動時の固まりを減らす）。
    // 空にすると毎回コンパイルする。
    void SetCacheDirectory(const std::filesystem::path& directory);

    // shaderRoot からの相対パスを指定する。失敗時は nullptr を返し、エラーはログへ出す。
    ComPtr<IDxcBlob> Compile(const std::wstring& relativePath, const wchar_t* entryPoint,
                             const wchar_t* targetProfile);

    // 監視対象のシェーダファイルに更新があれば true を返す。初回呼び出しは常に false。
    bool PollChanges();

    const std::filesystem::path& Root() const { return m_root; }

private:
    // shaders/ 配下の .hlsli をまとめたダイジェスト。どのシェーダがどれを include するかは
    // 追わず、共通ヘッダが 1 つでも変わればキャッシュを全部作り直す。
    uint64_t IncludeDigest();
    ComPtr<IDxcBlob> LoadCachedBlob(const std::filesystem::path& path) const;
    void StoreCachedBlob(const std::filesystem::path& path, IDxcBlob* blob) const;

    void ScanTimestamps(std::unordered_map<std::wstring, std::filesystem::file_time_type>& out) const;

    ComPtr<IDxcUtils> m_utils;
    ComPtr<IDxcCompiler3> m_compiler;
    ComPtr<IDxcIncludeHandler> m_includeHandler;
    std::filesystem::path m_root;
    std::unordered_map<std::wstring, std::filesystem::file_time_type> m_timestamps;
    bool m_scanned = false;
    std::filesystem::path m_cacheDirectory;
    uint64_t m_includeDigest = 0;
    bool m_includeDigestValid = false;
    // ホットリロードの検出とは別に持つ。プロファイルや引数が同じでもコンパイラが
    // 変われば出力が変わるので、キーに混ぜる。
    std::wstring m_compilerVersion;
};

}  // namespace tg::rhi
