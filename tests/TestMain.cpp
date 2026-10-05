// テストの入口。領域ごとの関数を順に呼ぶだけ。
//
// 対象は「スクリーンショットでは確認できないもの」。
// UI の相互作用（ドラッグ、ホバー）、アンドゥ履歴の段のまとめ方、
// フレームレート上限の待ち時間。

#include "TestSupport.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

void RunLayerMaterialTests();
void RunPieceTests();
void RunMeshVolumeTests();
void RunRockTests();
void RunBaseRockTests();
void RunVolumeTests();
void RunVolumeBooleanTests();
void RunPlaneCutsTests();
void RunVolumeCrackTests();
void RunVolumeNoiseTests();
void RunVolumeSmoothTests();
void RunVolumeEdgeWearTests();
void RunVolumeClipTests();
void RunVolumeScatterTests();
void RunVolumeUndercutTests();
void RunVolumeErodeTests();
void RunFlowMaskTests();
void RunVolumeTerraceTests();
void RunVolumeCloseTests();
void RunDecimateTests();
void RunRemeshTests();
void RunRockAssetTests();
void RunDetailTransferTests();
void RunTerrainTests();
void RunTerrainErodeTests();
void RunRockAssetIoTests();
void RunRockScatterTests();
void RunUvTests();
void RunShadowCascadeTests();
void RunMeshSceneTests();
void RunFrameLimiterTests();
void RunNodeGraphTests();
void RunGraphIoTests();
void RunRockTemplatesTests();
void RunNodeParamsTests();
void RunUiInteractionTests();
void RunUndoHistoryTests();
void RunProjectWorkspaceTests();

void RunApplyMaterialTests();
void RunDisplaceTests();
void RunShapeMaskTests();
void RunCurvatureMaskTests();
void RunNoiseMaskTests();
void RunStructureMaskTests();
void RunVolumeDiffMaskTests();
void RunDepositionMaskTests();
void RunMaskCombineTests();
void RunMaskFilterTests();
#include <string>
#include <vector>

namespace {
struct Group {
    const char* name;
    void (*run)();
};
// 実行の順は従来の全テストと同じ。名前はファイル名（<名前>Tests.cpp）から Tests を除いたもの。
const Group kGroups[] = {
    {"Displace", &RunDisplaceTests},
    {"LayerMaterial", &RunLayerMaterialTests},
    {"ApplyMaterial", &RunApplyMaterialTests},
    {"ShapeMask", &RunShapeMaskTests},
    {"CurvatureMask", &RunCurvatureMaskTests},
    {"NoiseMask", &RunNoiseMaskTests},
    {"StructureMask", &RunStructureMaskTests},
    {"VolumeDiffMask", &RunVolumeDiffMaskTests},
    {"DepositionMask", &RunDepositionMaskTests},
    {"MaskCombine", &RunMaskCombineTests},
    {"MaskFilter", &RunMaskFilterTests},
    {"Piece", &RunPieceTests},
    {"MeshVolume", &RunMeshVolumeTests},
    {"Rock", &RunRockTests},
    {"BaseRock", &RunBaseRockTests},
    {"Volume", &RunVolumeTests},
    {"VolumeBoolean", &RunVolumeBooleanTests},
    {"PlaneCuts", &RunPlaneCutsTests},
    {"VolumeCrack", &RunVolumeCrackTests},
    {"VolumeNoise", &RunVolumeNoiseTests},
    {"VolumeSmooth", &RunVolumeSmoothTests},
    {"VolumeEdgeWear", &RunVolumeEdgeWearTests},
    {"VolumeClip", &RunVolumeClipTests},
    {"VolumeScatter", &RunVolumeScatterTests},
    {"VolumeUndercut", &RunVolumeUndercutTests},
    {"VolumeErode", &RunVolumeErodeTests},
    {"FlowMask", &RunFlowMaskTests},
    {"VolumeTerrace", &RunVolumeTerraceTests},
    {"VolumeClose", &RunVolumeCloseTests},
    {"Decimate", &RunDecimateTests},
    {"Remesh", &RunRemeshTests},
    {"RockAsset", &RunRockAssetTests},
    {"DetailTransfer", &RunDetailTransferTests},
    {"Terrain", &RunTerrainTests},
    {"TerrainErode", &RunTerrainErodeTests},
    {"RockAssetIo", &RunRockAssetIoTests},
    {"RockScatter", &RunRockScatterTests},
    {"Uv", &RunUvTests},
    {"ShadowCascade", &RunShadowCascadeTests},
    {"MeshScene", &RunMeshSceneTests},
    {"UiInteraction", &RunUiInteractionTests},
    {"UndoHistory", &RunUndoHistoryTests},
    {"ProjectWorkspace", &RunProjectWorkspaceTests},
    {"FrameLimiter", &RunFrameLimiterTests},
    {"NodeGraph", &RunNodeGraphTests},
    {"GraphIo", &RunGraphIoTests},
    {"RockTemplates", &RunRockTemplatesTests},
    {"NodeParams", &RunNodeParamsTests},
};

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

void PrintUsage() {
    std::printf("rock_editor_tests [--only <名前>[,<名前>...]] [--list]\n"
                "  --only  名前（大文字小文字は区別しない。部分一致）に合う群だけを実行する。例: --only RockScatter,MaskFilter\n"
                "  --list  群の名前を一覧する\n"
                "  旧: --node-params-only / --displace-only も使える\n");
}
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--list") {
            for (const Group& group : kGroups) std::printf("%s\n", group.name);
            return 0;
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        } else if (argument == "--node-params-only") {
            only.push_back("nodeparams");
        } else if (argument == "--displace-only") {
            only.push_back("displace");
        } else if (argument == "--only" && i + 1 < argc) {
            std::string list = argv[++i];
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const std::string item = Lower(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!item.empty()) only.push_back(item);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else {
            std::printf("知らない引数: %s\n", argument.c_str());
            PrintUsage();
            return 2;
        }
    }
    struct Timing {
        const char* name;
        double seconds;
        int failures;
    };
    std::vector<Timing> timings;
    const auto suiteStart = std::chrono::steady_clock::now();
    int ran = 0;
    for (const Group& group : kGroups) {
        bool selected = only.empty();
        const std::string lower = Lower(group.name);
        for (const std::string& item : only)
            if (lower == item || lower.find(item) != std::string::npos) selected = true;
        if (!selected) continue;
        // リダイレクト中も群の開始・完了をすぐ出し、長い評価の待ち場所を分かるようにする。
        std::printf("\n[ RUN  ] %s\n", group.name);
        std::fflush(stdout);
        const int failuresBefore = rock::tests::g_failures;
        const auto start = std::chrono::steady_clock::now();
        group.run();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const int failures = rock::tests::g_failures - failuresBefore;
        timings.push_back({group.name, seconds, failures});
        std::printf("[ DONE ] %s: %.3f s, failures=%d\n", group.name, seconds, failures);
        std::fflush(stdout);
        ++ran;
    }
    if (ran == 0) {
        std::printf("該当する群がありません（--list で名前を確認）\n");
        return 2;
    }
    if (!only.empty()) std::printf("\n（%d 群を実行。コミット前は全テストを実行する）\n", ran);
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - suiteStart).count();
    std::stable_sort(timings.begin(), timings.end(), [](const Timing& a, const Timing& b) {
        return a.seconds > b.seconds;
    });
    std::printf("\n群ごとの所要時間（遅い順）:\n");
    for (const Timing& timing : timings)
        std::printf("  %-20s %9.3f s  failures=%d\n", timing.name, timing.seconds, timing.failures);
    std::printf("合計: %d 群, %.3f s\n", ran, total);
    std::printf("\n%s\n", (rock::tests::g_failures == 0) ? "すべて成功" : "失敗あり");
    return (rock::tests::g_failures == 0) ? 0 : 1;
}
