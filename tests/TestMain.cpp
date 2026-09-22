// テストの入口。領域ごとの関数を順に呼ぶだけ。
//
// 対象は「スクリーンショットでは確認できないもの」。
// UI の相互作用（ドラッグ、ホバー）、アンドゥ履歴の段のまとめ方、
// フレームレート上限の待ち時間。

#include "TestSupport.h"
#include <cstring>

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
void RunVolumeTerraceTests();
void RunVolumeCloseTests();
void RunDecimateTests();
void RunRemeshTests();
void RunUvTests();
void RunShadowCascadeTests();
void RunMeshSceneTests();
void RunFrameLimiterTests();
void RunNodeGraphTests();
void RunUiInteractionTests();
void RunUndoHistoryTests();
void RunProjectWorkspaceTests();

void RunApplyMaterialTests();
void RunDisplaceTests();
void RunShapeMaskTests();
void RunMaskCombineTests();
int main(int argc, char** argv) {
    RunDisplaceTests();
    if (argc == 2 && std::strcmp(argv[1], "--displace-only") == 0)
        return rock::tests::g_failures == 0 ? 0 : 1;
    RunApplyMaterialTests();
    RunShapeMaskTests();
    RunMaskCombineTests();
    RunPieceTests();
    RunMeshVolumeTests();
    RunRockTests();
    RunBaseRockTests();
    RunVolumeTests();
    RunVolumeBooleanTests();
    RunPlaneCutsTests();
    RunVolumeCrackTests();
    RunVolumeNoiseTests();
    RunVolumeSmoothTests();
    RunVolumeTerraceTests();
    RunVolumeCloseTests();
    RunDecimateTests();
    RunRemeshTests();
    RunUvTests();
    RunShadowCascadeTests();
    RunMeshSceneTests();
    RunUiInteractionTests();
    RunUndoHistoryTests();
    RunProjectWorkspaceTests();
    RunFrameLimiterTests();
    RunNodeGraphTests();

    std::printf("\n%s\n", (rock::tests::g_failures == 0) ? "すべて成功" : "失敗あり");
    return (rock::tests::g_failures == 0) ? 0 : 1;
}
