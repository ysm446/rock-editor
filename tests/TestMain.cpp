// テストの入口。領域ごとの関数を順に呼ぶだけ。
//
// 対象は「スクリーンショットでは確認できないもの」。
// UI の相互作用（ドラッグ、ホバー）、アンドゥ履歴の段のまとめ方、
// フレームレート上限の待ち時間。

#include "TestSupport.h"
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
void RunVolumeTerraceTests();
void RunVolumeCloseTests();
void RunDecimateTests();
void RunRemeshTests();
void RunRockAssetTests();
void RunDetailTransferTests();
void RunTerrainTests();
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
void RunDepositionMaskTests();
void RunMaskCombineTests();
void RunMaskFilterTests();
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--node-params-only") == 0) {
        RunNodeParamsTests();
        return rock::tests::g_failures == 0 ? 0 : 1;
    }
    RunDisplaceTests();
    if (argc == 2 && std::strcmp(argv[1], "--displace-only") == 0)
        return rock::tests::g_failures == 0 ? 0 : 1;
    RunLayerMaterialTests();
    RunApplyMaterialTests();
    RunShapeMaskTests();
    RunCurvatureMaskTests();
    RunNoiseMaskTests();
    RunStructureMaskTests();
    RunDepositionMaskTests();
    RunMaskCombineTests();
    RunMaskFilterTests();
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
    RunVolumeEdgeWearTests();
    RunVolumeClipTests();
    RunVolumeScatterTests();
    RunVolumeUndercutTests();
    RunVolumeTerraceTests();
    RunVolumeCloseTests();
    RunDecimateTests();
    RunRemeshTests();
    RunRockAssetTests();
    RunDetailTransferTests();
    RunTerrainTests();
    RunRockAssetIoTests();
    RunRockScatterTests();
    RunUvTests();
    RunShadowCascadeTests();
    RunMeshSceneTests();
    RunUiInteractionTests();
    RunUndoHistoryTests();
    RunProjectWorkspaceTests();
    RunFrameLimiterTests();
    RunNodeGraphTests();
    RunGraphIoTests();
    RunRockTemplatesTests();
    RunNodeParamsTests();

    std::printf("\n%s\n", (rock::tests::g_failures == 0) ? "すべて成功" : "失敗あり");
    return (rock::tests::g_failures == 0) ? 0 : 1;
}
