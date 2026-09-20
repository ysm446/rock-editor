#include "rhi/ShaderCompiler.h"

#include "core/Log.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

namespace tg::rhi {
namespace {

bool IsShaderFile(const std::filesystem::path& path) {
    const std::filesystem::path ext = path.extension();
    return ext == L".hlsl" || ext == L".hlsli";
}

bool IsIncludeFile(const std::filesystem::path& path) {
    return path.extension() == L".hlsli";
}

// キャッシュファイルの形式を変えたら上げる。古いファイルは名前が合わなくなるだけで、
// 消す処理は持たない（数十 KB のファイルが残るだけなので）。
constexpr uint64_t kCacheFormatVersion = 1;

// FNV-1a 64bit。暗号学的な強さは要らない。同じソースと引数なら同じ名前になればよい。
struct Fnv1a64 {
    uint64_t value = 14695981039346656037ull;

    void Add(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            value ^= bytes[i];
            value *= 1099511628211ull;
        }
    }
    void Add(const std::wstring& text) {
        Add(text.data(), text.size() * sizeof(wchar_t));
        Add("|", 1);
    }
    void Add(uint64_t number) { Add(&number, sizeof(number)); }
};

bool ReadWholeFile(const std::filesystem::path& path, std::vector<char>& out) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return false;
    }
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    stream.seekg(0);
    if (size > 0 && !stream.read(out.data(), size)) {
        return false;
    }
    return true;
}

}  // namespace

ShaderCompiler::~ShaderCompiler() {
    Destroy();
}

bool ShaderCompiler::Create(const std::filesystem::path& shaderRoot) {
    m_root = shaderRoot;

    std::error_code ec;
    if (!std::filesystem::is_directory(m_root, ec)) {
        TG_LOG_ERROR("シェーダディレクトリが見つかりません: %ls", m_root.c_str());
        return false;
    }

    if (!TG_CHECK_HR(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_utils)))) {
        return false;
    }
    if (!TG_CHECK_HR(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_compiler)))) {
        return false;
    }
    if (!TG_CHECK_HR(m_utils->CreateDefaultIncludeHandler(&m_includeHandler))) {
        return false;
    }

    // キャッシュのキーへ混ぜるコンパイラのバージョン。取れなければ 0.0 として扱う
    // （その場合は DXC を入れ替えてもキャッシュが当たるので、手で消してもらう）。
    {
        UINT32 major = 0;
        UINT32 minor = 0;
        ComPtr<IDxcVersionInfo> version;
        if (SUCCEEDED(m_compiler.As(&version)) && version) {
            version->GetVersion(&major, &minor);
        }
        m_compilerVersion = std::to_wstring(major) + L"." + std::to_wstring(minor);
    }
    m_includeDigestValid = false;

    TG_LOG_INFO("シェーダディレクトリ: %ls", m_root.c_str());
    return true;
}

void ShaderCompiler::SetCacheDirectory(const std::filesystem::path& directory) {
    m_cacheDirectory = directory;
    if (m_cacheDirectory.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_cacheDirectory, ec);
    if (ec) {
        TG_LOG_WARN("シェーダキャッシュのフォルダを作れません: %ls。毎回コンパイルします",
                    m_cacheDirectory.c_str());
        m_cacheDirectory.clear();
        return;
    }
    TG_LOG_INFO("シェーダキャッシュ: %ls", m_cacheDirectory.c_str());
}

uint64_t ShaderCompiler::IncludeDigest() {
    if (m_includeDigestValid) {
        return m_includeDigest;
    }
    // 順序で値が変わらないように、パスで並べてから足す。
    std::vector<std::filesystem::path> includes;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(m_root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && !fileEc && IsIncludeFile(it->path())) {
            includes.push_back(it->path());
        }
        it.increment(ec);
    }
    std::sort(includes.begin(), includes.end());

    Fnv1a64 hash;
    std::vector<char> bytes;
    for (const auto& path : includes) {
        hash.Add(std::filesystem::relative(path, m_root, ec).wstring());
        if (ReadWholeFile(path, bytes)) {
            hash.Add(bytes.data(), bytes.size());
        }
    }
    m_includeDigest = hash.value;
    m_includeDigestValid = true;
    return m_includeDigest;
}

ComPtr<IDxcBlob> ShaderCompiler::LoadCachedBlob(const std::filesystem::path& path) const {
    ComPtr<IDxcBlob> result;
    std::vector<char> bytes;
    if (!ReadWholeFile(path, bytes) || bytes.empty()) {
        return result;
    }
    ComPtr<IDxcBlobEncoding> blob;
    if (FAILED(m_utils->CreateBlob(bytes.data(), static_cast<UINT32>(bytes.size()), DXC_CP_ACP,
                                   &blob))) {
        return result;
    }
    result = blob;
    return result;
}

void ShaderCompiler::StoreCachedBlob(const std::filesystem::path& path, IDxcBlob* blob) const {
    // 途中まで書けたファイルを次回読まないように、別名で書いてから差し替える。
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open() ||
            !stream.write(static_cast<const char*>(blob->GetBufferPointer()),
                          static_cast<std::streamsize>(blob->GetBufferSize()))) {
            TG_LOG_WARN("シェーダキャッシュを書けません: %ls", path.c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
    }
}

void ShaderCompiler::Destroy() {
    m_includeHandler.Reset();
    m_compiler.Reset();
    m_utils.Reset();
    m_timestamps.clear();
    m_scanned = false;
    m_includeDigestValid = false;
}

ComPtr<IDxcBlob> ShaderCompiler::Compile(const std::wstring& relativePath,
                                         const wchar_t* entryPoint,
                                         const wchar_t* targetProfile) {
    ComPtr<IDxcBlob> result;
    if (!m_compiler) {
        return result;
    }

    const std::filesystem::path fullPath = m_root / relativePath;
    const std::wstring fullPathStr = fullPath.wstring();

    ComPtr<IDxcBlobEncoding> sourceBlob;
    if (!TG_CHECK_HR(m_utils->LoadFile(fullPathStr.c_str(), nullptr, &sourceBlob))) {
        TG_LOG_ERROR("シェーダを読み込めません: %ls", fullPathStr.c_str());
        return result;
    }

    DxcBuffer source = {};
    source.Ptr = sourceBlob->GetBufferPointer();
    source.Size = sourceBlob->GetBufferSize();
    // シェーダソースは BOM 無し UTF-8（日本語コメントを含む）。
    // ACP のままだと CP932 環境で多バイト列が誤解釈される。
    source.Encoding = DXC_CP_UTF8;

    const std::wstring includeArg = m_root.wstring();
    std::vector<const wchar_t*> extraArgs = {
        L"-I", includeArg.c_str(),
        L"-HV", L"2021",
        L"-enable-16bit-types",
    };
#if defined(TG_DEBUG)
    extraArgs.push_back(L"-Zi");
    extraArgs.push_back(L"-Qembed_debug");
    extraArgs.push_back(L"-Od");
#else
    extraArgs.push_back(L"-O3");
#endif

    // ディスクのキャッシュ。ソース・include・入口・プロファイル・引数・コンパイラの
    // バージョンから名前を決め、あればコンパイルせずに読む。
    std::filesystem::path cachePath;
    if (!m_cacheDirectory.empty()) {
        Fnv1a64 hash;
        hash.Add(kCacheFormatVersion);
        hash.Add(m_compilerVersion);
        hash.Add(relativePath);
        hash.Add(std::wstring(entryPoint));
        hash.Add(std::wstring(targetProfile));
        for (const wchar_t* arg : extraArgs) {
            // -I のパスは環境ごとに違うが、include の中身はダイジェストで見ているので外す。
            if (arg == includeArg.c_str()) continue;
            hash.Add(std::wstring(arg));
        }
        hash.Add(source.Ptr, source.Size);
        hash.Add(IncludeDigest());

        wchar_t name[32] = {};
        std::swprintf(name, sizeof(name) / sizeof(name[0]), L"%016llx.dxil",
                      static_cast<unsigned long long>(hash.value));
        cachePath = m_cacheDirectory / name;
        if (ComPtr<IDxcBlob> cached = LoadCachedBlob(cachePath); cached) {
            return cached;
        }
    }

    ComPtr<IDxcCompilerArgs> args;
    if (!TG_CHECK_HR(m_utils->BuildArguments(relativePath.c_str(), entryPoint, targetProfile,
                                             extraArgs.data(),
                                             static_cast<UINT32>(extraArgs.size()), nullptr, 0,
                                             &args))) {
        return result;
    }

    ComPtr<IDxcResult> compileResult;
    if (!TG_CHECK_HR(m_compiler->Compile(&source, args->GetArguments(), args->GetCount(),
                                         m_includeHandler.Get(),
                                         IID_PPV_ARGS(&compileResult)))) {
        return result;
    }

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors && errors->GetStringLength() > 0) {
        TG_LOG_WARN("%ls %ls:\n%s", relativePath.c_str(), entryPoint, errors->GetStringPointer());
    }

    HRESULT status = S_OK;
    compileResult->GetStatus(&status);
    if (FAILED(status)) {
        TG_LOG_ERROR("シェーダのコンパイルに失敗しました: %ls %ls", relativePath.c_str(),
                     entryPoint);
        return result;
    }

    if (!TG_CHECK_HR(compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&result), nullptr))) {
        return ComPtr<IDxcBlob>();
    }

    TG_LOG_INFO("シェーダをコンパイルしました: %ls %ls (%ls)", relativePath.c_str(), entryPoint,
                targetProfile);
    if (!cachePath.empty()) {
        StoreCachedBlob(cachePath, result.Get());
    }
    return result;
}

void ShaderCompiler::ScanTimestamps(
    std::unordered_map<std::wstring, std::filesystem::file_time_type>& out) const {
    // 例外は使わない方針のため、イテレータの増分も error_code 版で手動で回す
    // （range-for の operator++ は I/O エラー時に例外を投げる）。
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(m_root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code fileEc;
        if (entry.is_regular_file(fileEc) && !fileEc && IsShaderFile(entry.path())) {
            std::error_code timeEc;
            const auto writeTime = std::filesystem::last_write_time(entry.path(), timeEc);
            if (!timeEc) {
                out.emplace(entry.path().wstring(), writeTime);
            }
        }
        it.increment(ec);
    }
}

bool ShaderCompiler::PollChanges() {
    std::unordered_map<std::wstring, std::filesystem::file_time_type> current;
    ScanTimestamps(current);

    if (!m_scanned) {
        m_timestamps = std::move(current);
        m_scanned = true;
        return false;
    }

    bool changed = current.size() != m_timestamps.size();
    if (!changed) {
        for (const auto& [path, time] : current) {
            const auto it = m_timestamps.find(path);
            if (it == m_timestamps.end() || it->second != time) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        m_timestamps = std::move(current);
        // 共通ヘッダが変わったかもしれないので、次のコンパイルで取り直す。
        m_includeDigestValid = false;
    }
    return changed;
}

}  // namespace tg::rhi
