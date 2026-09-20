// テストの入口。領域ごとの関数を順に呼ぶだけ。
//
// 対象は「スクリーンショットでは確認できないもの」。
// UI の相互作用（ドラッグ、ホバー）、アンドゥ履歴の段のまとめ方、
// フレームレート上限の待ち時間。

#include "TestSupport.h"

void RunRoadTests();
void RunShadowCascadeTests();
void RunConnectionPrototypeTests();
void RunSurfaceLayoutTests();
void RunMeshSceneTests();
void RunFrameLimiterTests();
void RunNodeGraphTests();
void RunUiInteractionTests();
void RunUndoHistoryTests();
void RunProjectWorkspaceTests();

int main() {
    RunRoadTests();
    RunShadowCascadeTests();
    RunConnectionPrototypeTests();
    RunSurfaceLayoutTests();
    RunMeshSceneTests();
    RunUiInteractionTests();
    RunUndoHistoryTests();
    RunProjectWorkspaceTests();
    RunFrameLimiterTests();
    RunNodeGraphTests();

    std::printf("\n%s\n", (tg::tests::g_failures == 0) ? "すべて成功" : "失敗あり");
    return (tg::tests::g_failures == 0) ? 0 : 1;
}
