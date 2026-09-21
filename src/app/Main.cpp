#include "app/Application.h"

#include "core/Log.h"
#include "core/PathUtf8.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <string>

// --- DirectX 12 Agility SDK ------------------------------------------------
// 実行ファイルからエクスポートすることで、D3D12/ 配下の新しいランタイムが使われる。
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

// 使い方:
//   rock_editor.exe [--root <dir>] [--project <path>] [--save-project <path>]
//                       [--hdri <path>] [--texture <path>]...
//                       [--screenshot <path>] [--screenshot-ui <path>]
//                       [--screenshot-frame <n>] [--import-model <fbx>] [--open-asset <path>] [--place-model <path>] [--gizmo-rotate] [--gizmo-scale]
//                       [--model-node-rotation <node> <x> <y> <z>] [--model-node-gizmo <node>] [--focus-panel <name>]
rock::StartupOptions ParseCommandLine() {
    rock::StartupOptions options;

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return options;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--project" && (i + 1) < argc) {
            options.projectPath = argv[i + 1];
            ++i;
        } else if (argument == L"--root" && (i + 1) < argc) {
            options.projectRoot = argv[++i];
        } else if (argument == L"--inspect-asset-delete" && (i + 1) < argc) {
            options.inspectAssetDelete = argv[++i];
        } else if (argument == L"--measure-preview") {
            options.measurePreview = true;
        } else if (argument == L"--test-layer-thumbnail-cache") {
            options.testLayerThumbnailCache = true;
        } else if (argument == L"--test-drag" && (i + 4) < argc) {
            options.testDrag = true;
            options.testDragStart.x = static_cast<float>(::_wtof(argv[++i]));
            options.testDragStart.y = static_cast<float>(::_wtof(argv[++i]));
            options.testDragEnd.x = static_cast<float>(::_wtof(argv[++i]));
            options.testDragEnd.y = static_cast<float>(::_wtof(argv[++i]));
        } else if (argument == L"--test-viewport-gesture" && (i + 1) < argc) {
            options.testViewportGesture = ::_wtoi(argv[++i]);
        } else if (argument == L"--test-drag-shift") {
            options.testDragShift = true;
        } else if (argument == L"--test-drag-cancel") {
            options.testDragCancel = true;
        } else if (argument == L"--test-double-click") {
            options.testDoubleClick = true;
        } else if (argument == L"--test-click-after-drag" && (i + 2) < argc) {
            options.testClickAfterDrag = true;
            options.testClickPosition.x = float(::_wtof(argv[++i]));
            options.testClickPosition.y = float(::_wtof(argv[++i]));
        } else if (argument == L"--test-delete") {
            options.testDelete = true;
        } else if (argument == L"--select-node" && (i + 1) < argc) {
            options.selectNode = ::_wtoi(argv[++i]);
        } else if (argument == L"--bake-node" && (i + 1) < argc) {
            options.bakeNode = ::_wtoi(argv[++i]);
            options.exportBakeNode = options.bakeNode;
        } else if (argument == L"--export-bake" && (i + 1) < argc) {
            options.exportBakeDirectory = argv[++i];
        } else if (argument == L"--preview-node" && (i + 1) < argc) {
            options.previewNode = ::_wtoi(argv[++i]);
        } else if (argument == L"--import-model" && (i + 1) < argc) {
            options.importModel = argv[++i];
        } else if (argument == L"--gizmo-rotate") {
            options.gizmoRotate = true;
        } else if (argument == L"--gizmo-scale") {
            options.gizmoScale = true;
        } else if (argument == L"--model-node-rotation" && (i + 4) < argc) {
            rock::renderer::ModelNodeRotation rotation;
            rotation.node = rock::ToUtf8Display(std::filesystem::path(argv[++i]));
            for (float& degrees : rotation.rotationDegrees) degrees = static_cast<float>(::_wtof(argv[++i]));
            options.modelNodeRotations.push_back(std::move(rotation));
        } else if (argument == L"--focus-panel" && (i + 1) < argc) {
            options.focusPanel = rock::ToUtf8Display(std::filesystem::path(argv[++i]));
        } else if (argument == L"--model-node-gizmo" && (i + 1) < argc) {
            options.modelNodeGizmo = rock::ToUtf8Display(std::filesystem::path(argv[++i]));
        } else if (argument == L"--place-model" && (i + 1) < argc) {
            options.placeModel = argv[++i];
        } else if (argument == L"--open-asset" && (i + 1) < argc) {
            options.openAsset = argv[++i];
        } else if (argument == L"--save-project" && (i + 1) < argc) {
            options.saveProjectPath = argv[i + 1];
            ++i;
        } else if (argument == L"--hdri" && (i + 1) < argc) {
            options.hdriPath = argv[i + 1];
            ++i;
        } else if (argument == L"--texture" && (i + 1) < argc) {
            options.texturePaths.emplace_back(argv[i + 1]);
            ++i;
        } else if (argument == L"--screenshot" && (i + 1) < argc) {
            options.screenshotPath = argv[i + 1];
            ++i;
        } else if (argument == L"--screenshot-ui" && (i + 1) < argc) {
            options.uiScreenshotPath = argv[i + 1];
            ++i;
        } else if (argument == L"--test-gpu-ao") {
            options.testGpuAo = true;
        } else if (argument == L"--screenshot-frame" && (i + 1) < argc) {
            options.screenshotFrame = static_cast<uint32_t>(::_wtoi(argv[i + 1]));
            ++i;
        } else {
            ROCK_LOG_WARN("不明な引数です: %ls", argument.c_str());
        }
    }

    ::LocalFree(argv);
    return options;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const rock::StartupOptions options = ParseCommandLine();

    rock::Application app;
    if (!app.Initialize(options)) {
        app.Shutdown();
        return 1;
    }

    const int result = app.Run();
    app.Shutdown();
    return result;
}
