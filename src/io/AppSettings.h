#pragma once
#include "geometry/Volume.h"

#include <filesystem>

namespace rock::io {

// アプリ側の状態を置くフォルダ（`%LOCALAPPDATA%/rock-editor`）。
// プロジェクトの中身ではないもの（設定、最近使ったファイル）はここへ置く。
// 環境変数が引けないときは作業ディレクトリを返す。
std::filesystem::path AppDataDirectory();

// UI の見た目に関する設定。プロジェクトではなくアプリに紐づく。
struct UiSettings {
    // Windows の表示スケール（DPI）に合わせて UI を拡大するか。
    //
    // 既定は**合わせない**。クライアント領域を実ピクセルで固定しているので、
    // 追従させると作業面積がモニタ設定によって変わる（design-guide.md の「寸法と DPI」）。
    // 高 DPI で UI が小さすぎるときに、使う人が選べるようにしてある。
    bool followSystemScale = false;
    // 追従しないときの拡大率。
    float manualScale = 1.0f;
    // 文字の基準サイズ（px、拡大率を掛ける前）。UI 全体に効く。
    // 拡大率と違って余白や部品幅は動かないので、**文字だけを詰めたい / 大きくしたい**
    // ときに使う。既定と範囲は `ui/UiStyle.h` の kDefaultFontSize / kMinFontSize / kMaxFontSize と揃える。
    int fontSize = 17;
    // レイヤーパネルの一覧側（上の区画）の高さ（96 DPI 基準）。
    // 境界のドラッグで変わる。**拡大率を掛ける前の値で持つ**ので、
    // 表示スケールを変えても区画の見た目の高さが保たれる。
    float layerListHeight = 260.0f;
    // アセットの帯のフォルダ階層（左の区画）の幅。同じく拡大率を掛ける前の値。
    float assetFolderWidth = 190.0f;
};

// 表示に関する設定（設定ウィンドウの「表示」節）。
// design-guide の「設定ウィンドウ」が settings.json に置くと定めているもの。
struct DisplaySettings {
    bool vsync = true;
    bool hotReload = true;
    // ビューポートの右上に FPS を出すか。
    bool showFps = false;
    // ビューポートの右上に描画の量（ドローコール・頂点・三角形）を出すか。
    bool showStats = false;
    // 原点中心の50m四方、1m刻みの作業グリッド。
    bool showReferenceGrid = true;
    bool showUvChecker = false;
    // 陰影の上に三角形の辺を黒い線で重ねるか。表示モードは変えない。
    bool showWireframeOverlay = false;
    // 岩メッシュの法線を、折れ角つきで平均して滑らかに見せるか。偽なら面法線（面ごとの陰影）。
    bool smoothShading = false;
    // Volumeを直接表示するときだけ使用。Volume to Meshの出力方式とは独立。
    geometry::VolumeMeshingMethod sdfPreviewMethod = geometry::VolumeMeshingMethod::MarchingTetrahedra;
    // アセットの帯（ルートのフォルダ階層とその中身）を出すか。畳むとビューポートが縦に広がる。
    bool showAssetBand = true;
    // 前面にあるときの FPS 上限。0 で上限なし。
    int frameRateLimit = 0;
    // **背面にあるときの FPS 上限。** 見えていない絵に GPU を回し続けないため、
    // 既定で低く抑える。0 で上限なし。
    int inactiveFrameRateLimit = 10;
    float clearColor[4] = {0.09f, 0.09f, 0.11f, 1.0f};
};

// 設定ファイル（`settings.json`）の読み書き。
//
// 値を変えたら Save() を呼ぶ。書けなくても動作は続ける（次回に持ち越せないだけ）。
class AppSettings {
public:
    void Load();
    bool Save() const;

    UiSettings& Ui() { return m_ui; }
    const UiSettings& Ui() const { return m_ui; }

    DisplaySettings& Display() { return m_display; }
    const DisplaySettings& Display() const { return m_display; }

private:
    UiSettings m_ui;
    DisplaySettings m_display;
};

}  // namespace rock::io
