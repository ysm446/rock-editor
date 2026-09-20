// プレビュー設定パネルと「ライティング」パネル。
// どちらも道路の中身ではなく、見え方（レンダラ側の設定）を扱う。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

void Application::DrawMaterialPanel() {
    // **ここでは前面を要求しない。** レイヤーと同じ枠のタブなので、
    // 両方が要求すると後から描いたほうが勝ち、既定の前面が定まらない。
    if (ImGui::Begin("プレビュー設定")) {
        ui::SectionHeader("メッシュ");
        if (ui::BeginPropertyTable("previewMeshRows")) {
            // 分割の仕方をここで決める。
            ui::PropertyBool("テセレーション", &m_renderer.TessellationEnabled(),
                             renderer::kPreviewDefaults.tessellationEnabled,
                             "画面上の辺が長いところだけメッシュを細かく割る。"
                             "変位量を上げたときに形がなめらかになる。分割後の辺は"
                             "表示メニューのワイヤーフレームで確認できる");
            if (m_renderer.TessellationEnabled()) {
                ui::PropertyFloat("分割の上限", &m_renderer.TessellationFactor(), 1.0f, 64.0f,
                                  renderer::kPreviewDefaults.tessellationFactor,
                                  "1 辺をこの回数まで割る。1 m のセルは 16 で約 6 cm、64 で約 1.5 cm。"
                                  "遠くは画面上の辺が短いので割らない", "%.0f", 0, 1.0f);
                ui::PropertyFloat("分割する辺の長さ", &m_renderer.TessellationTargetPixels(), 4.0f, 32.0f,
                                  renderer::kPreviewDefaults.tessellationTargetPixels,
                                  "画面上で 1 辺がこの長さ（px）を超えたら割る。小さいほど細かく、負荷は二乗で増える",
                                  "%.0f px", 0, 1.0f);
            }
            // 道路の材質（スロット 1〜4）を合成する解像度。タイル 1 枚ぶんなので、反復長が短いほど細部が出る。
            int resolution = ResolutionIndex(m_renderer.MaterialResolution());
            if (ui::PropertyCombo("合成解像度", &resolution, kResolutionLabels, IM_ARRAYSIZE(kResolutionLabels),
                                  ResolutionIndex(renderer::kPreviewDefaults.materialResolution),
                                  "道路のマテリアルを合成する解像度（タイル 1 枚ぶん）。上げるほど細部が出るが重くなる")) {
                m_renderer.RequestMaterialResolution(kResolutionValues[resolution]);
            }
            ui::EndPropertyTable();
        }

        ui::SectionHeader("影");
        if (ui::BeginPropertyTable("shadowRows")) {
            ui::PropertyBool("有効", &m_renderer.ShadowEnabled(), renderer::kPreviewDefaults.shadowEnabled,
                "太陽の影を表示する。素材のハイトによる凹凸も影に反映する");
            int cascades = static_cast<int>(m_renderer.ShadowCascadeCount());
            if (ui::PropertyInt("カスケード数", &cascades, 1, static_cast<int>(renderer::kShadowCascadeCount),
                                static_cast<int>(renderer::kPreviewDefaults.shadowCascadeCount),
                                "影を描くカメラの数。1 はシーン全体を 1 枚で覆う軽量な方式。"
                                "2 以上は視距離を分けて近景に解像度を重点配分し、増やすほど描画負荷とメモリも増える"))
                m_renderer.RequestShadowCascadeCount(static_cast<uint32_t>(cascades));
            const char* labels[] = {"1024 × 1024", "2048 × 2048", "4096 × 4096"};
            const uint32_t values[] = {1024, 2048, 4096};
            int selected = 0, defaultIndex = 0;
            for (int i = 0; i < 3; ++i) {
                if (values[i] == m_renderer.ShadowResolution()) selected = i;
                if (values[i] == renderer::kPreviewDefaults.shadowResolution) defaultIndex = i;
            }
            if (ui::PropertyCombo("解像度", &selected, labels, 3, defaultIndex,
                "シャドウマップ1枚あたりの解像度。解像度を2倍にすると画素数と必要なメモリは4倍になる"))
                m_renderer.RequestShadowResolution(values[selected]);
            ui::EndPropertyTable();
        }

        ui::SectionHeader("カメラ");
        if (ui::BeginPropertyTable("cameraRows")) {
            // 露出を絞り / シャッター / ISO で決めているので、レンズも同じ言葉で扱う。
            // ラジアンのままだと何 mm 相当なのか分からない。
            renderer::Camera& camera = m_renderer.GetCamera();
            float focalLength = renderer::FocalLengthFromFovY(camera.FovY());
            if (ui::PropertyFloat("焦点距離", &focalLength, 12.0f, 200.0f,
                                  renderer::FocalLengthFromFovY(kDefaultCamera.fovY),
                                  "35mm フルサイズ換算。小さいほど広角で、遠近が強く出る",
                                  "%.0f mm", ImGuiSliderFlags_Logarithmic, 1.0f)) {
                camera.SetFovY(renderer::FovYFromFocalLength(focalLength));
            }
            // **F 値はレンズの値なのでここに置く。** 露出（絞り）とボケの
            // どちらも同じ 1 つの値で決まる。EV を直接指定していると露出の節から
            // 絞りの行が消えるので、レンズ側に置いておかないと触れなくなる。
            ui::PropertyFloat("F 値", &m_renderer.Exposure().aperture, 1.0f, 32.0f,
                              renderer::ExposureSettings{}.aperture,
                              "絞り。小さいほどボケが強く、露出は明るくなる", "F%.1f",
                              ImGuiSliderFlags_Logarithmic);
            ui::PropertyValue("画角", "%.1f 度（垂直）", RadiansToDegrees(camera.FovY()));
            ui::PropertyLabelEmpty("cameraReset");
            if (ui::Button("視点をリセット", ui::kWideButtonWidth)) {
                m_renderer.GetCamera().Reset();
            }
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }

        // **レンズの値はここに出さない。** 焦点距離も F 値もカメラの節が持っていて、
        // すぐ上に見えている。同じ値を並べると、どちらが効いているのか分からなくなる。
        ui::SectionHeader("被写界深度");
        renderer::DofSettings& dof = m_renderer.Dof();
        const renderer::DofSettings kDefaultDof;
        if (ui::BeginPropertyTable("dofRows")) {
            ui::PropertyBool("有効", &dof.enabled, kDefaultDof.enabled,
                             "ビューポートの見え方だけに掛かる。マテリアルの合成には効かない");

            ImGui::BeginDisabled(!dof.enabled);

            ui::PropertyBool("注視点に追従", &dof.focusOnTarget, kDefaultDof.focusOnTarget,
                             "軌道カメラなので、見ているものは常に注視点にある。"
                             "切ると距離を手で決められる");
            ImGui::BeginDisabled(dof.focusOnTarget);
            ui::PropertyFloat("ピント距離", &dof.focusDistance, 0.1f, 50.0f,
                              kDefaultDof.focusDistance,
                              "カメラからピント面まで。ワールドの 1 単位を 1m とみなす",
                              "%.2f m", ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ui::PropertyValue("実効ピント距離", "%.2f m", m_renderer.FocusDistance());

            ui::PropertyFloat("ミニチュア", &dof.miniatureScale, 1.0f, 10000.0f,
                              kDefaultDof.miniatureScale,
                              "1 で実物大。上げるほど模型を撮った計算になり、"
                              "同じレンズでもボケが強くなる。実寸のままだと長い道路は"
                              "遠すぎて 1 画素もボケない。数百 m の道路を引きで見るなら "
                              "100〜1000 が目安",
                              "1 : %.0f", ImGuiSliderFlags_Logarithmic);
            ui::PropertyFloat("ボケの強さ", &dof.blurScale, 0.25f, 16.0f, kDefaultDof.blurScale,
                              "1 で現実どおり。2m 角の地面を広角で撮れば現実でも"
                              "全域にピントが合うので、見せたい量まで持ち上げるための誇張。"
                              "倍率を使わずに出したいなら望遠へ寄せる",
                              "x%.2f", ImGuiSliderFlags_Logarithmic);
            ui::PropertyFloat("最大ぼけ", &dof.maxBlurPixels, 1.0f, 64.0f,
                              kDefaultDof.maxBlurPixels,
                              "画面上のぼけ半径の上限。現実の式のままだと極端になるので、"
                              "表示のための頭打ちとして持つ。上げるほど重くなる",
                              "%.0f px");

            static const char* const kShapeLabels[] = {"円", "三角形", "六角形", "八角形"};
            int shape = static_cast<int>(dof.shape);
            if (ui::PropertyCombo("絞りの形", &shape, kShapeLabels, IM_ARRAYSIZE(kShapeLabels),
                                  static_cast<int>(kDefaultDof.shape), "ボケの形になる")) {
                dof.shape = static_cast<renderer::ApertureShape>(shape);
            }
            ImGui::BeginDisabled(dof.shape == renderer::ApertureShape::Circle);
            ui::PropertyFloat("絞りの向き", &dof.rotationDegrees, 0.0f, 180.0f,
                              kDefaultDof.rotationDegrees, "多角形のボケの角度", "%.0f 度");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ui::EndPropertyTable();
        }
        ui::HintText("ボケ量は焦点距離・F 値・ピント距離で決まる。ワールドの 1 単位 = 1m");
    }
    ImGui::End();
}

void Application::DrawLightingPanel() {
    if (ImGui::Begin("ライティング")) {
        // **作業用IBLとシーンの空は別々に持つ。** 切り替えてもどちらの設定も失わない。
        if (ui::BeginPropertyTable("lightingModeRows")) {
            static const char* const kModes[] = {"作業用IBL", "シーンの空"};
            int mode = m_renderer.AtmosphericMode() ? 1 : 0;
            if (ui::PropertyCombo("表示環境", &mode, kModes, IM_ARRAYSIZE(kModes), 0,
                                  "作業用IBLは質感の確認用（天球と作業用のライト）。\n"
                                  "シーンの空は大気散乱の空と太陽で照らす。太陽の色と空の明るさは大気の計算で決まる")) {
                m_renderer.AtmosphericMode() = mode == 1;
            }
            ui::EndPropertyTable();
        }
        const bool atmospheric = m_renderer.AtmosphericMode();
        if (!atmospheric) ui::HintText("作業用IBLで確認中。シーンの太陽・大気の設定は保持されている");
        renderer::LightSettings& light = m_renderer.Light();
        const renderer::AtmosphereSettings skyDefaults;

        ui::SectionHeader(atmospheric ? "シーンの太陽" : "作業用ライト");
        if (ui::BeginPropertyTable("lightRows")) {
            float azimuthDeg = RadiansToDegrees(light.azimuth);
            if (ui::PropertyFloat("方位角", &azimuthDeg, -180.0f, 180.0f,
                                  RadiansToDegrees(atmospheric ? skyDefaults.azimuth : kDefaultLight.azimuth),
                                  "太陽の向き（水平方向）", "%.0f 度")) {
                light.azimuth = DegreesToRadians(azimuthDeg);
            }
            float elevationDeg = RadiansToDegrees(light.elevation);
            if (ui::PropertyFloat("仰角", &elevationDeg, atmospheric ? -10.0f : -89.0f, 89.0f,
                                  RadiansToDegrees(atmospheric ? skyDefaults.elevation : kDefaultLight.elevation),
                                  atmospheric ? "太陽の高さ。低いほど影が伸び、光と空が赤くなる。地平線より下では直射光が無くなる"
                                              : "太陽の高さ。低いほど影が伸びる",
                                  "%.0f 度")) {
                light.elevation = DegreesToRadians(elevationDeg);
            }
            if (atmospheric) {
                ui::PropertyFloat("照度", &light.illuminance, 0.0f, 200000.0f, skyDefaults.illuminance,
                                  "大気圏外の照度（lux）。地表では大気の透過率で減衰する", "%.0f");
                const renderer::LightSettings effective = m_renderer.EffectiveLight();
                ui::PropertyValue("光の色", "大気の透過率から自動計算");
                ui::PropertyValue("地表の照度", "%.0f lux",
                                  effective.illuminance *
                                      (0.2126f * effective.color.x + 0.7152f * effective.color.y + 0.0722f * effective.color.z));
            } else {
                ui::PropertyFloat("照度", &light.illuminance, 0.0f, 200000.0f,
                                  kDefaultLight.illuminance,
                                  "lux。晴天の直射日光がおよそ 100000 lux", "%.0f");
                ui::PropertyColorLinear("光の色", &light.color.x, &kDefaultLight.color.x);
            }
            ui::EndPropertyTable();
        }

        if (atmospheric) {
            renderer::AtmosphereSettings& sky = m_renderer.AtmosphericSettings();
            ui::SectionHeader("大気");
            if (ui::BeginPropertyTable("atmosphereRows", 150.0f)) {
                ui::PropertyFloat("レイリー散乱強度", &sky.density, 0.1f, 3.0f, skyDefaults.density,
                                  "空の青さや夕焼けを生む大気の散乱量。1 が地球の標準。\n"
                                  "大きくすると散乱と太陽光の減衰が強くなり、空の色と照明が変わる", "%.2f");
                ui::PropertyFloat("ミー密度", &sky.mie, 0.0f, 2.0f, skyDefaults.mie,
                                  "エアロゾル（霞・塵）の密度の倍率。大きくすると太陽の周囲や地平線が白っぽく霞み、直射光が弱まる",
                                  "%.2f");
                ui::PropertyFloat("ミー異方性", &sky.eccentricity, 0.0f, 0.95f, skyDefaults.eccentricity,
                                  "ミー散乱の異方性（g）。大きいほど太陽付近の光が鋭く集中する", "%.2f");
                ui::PropertyFloat("地表の基準標高", &sky.altitude, 0.0f, 10000.0f, skyDefaults.altitude,
                                  "道路の原点の海抜高度（m）。空と光を計算する基準で、道路やカメラは動かない", "%.0f m");
                int lowerHemisphere = static_cast<int>(sky.lowerHemisphere);
                static const char* const kLowerHemisphereLabels[] = {"空の延長", "地面反射"};
                if (ui::PropertyCombo("下半球", &lowerHemisphere, kLowerHemisphereLabels, 2,
                                      static_cast<int>(skyDefaults.lowerHemisphere),
                                      "地平線より下の見え方。空の延長は青空を下へ折り返す見た目のための近似。\n"
                                      "地面反射は日光と空の光を受けた地表の反射。背景と環境光の両方に効く")) {
                    sky.lowerHemisphere = static_cast<uint32_t>(lowerHemisphere);
                }
                ui::PropertyFloat("グラウンドアルベド", &sky.groundAlbedo, 0.0f, 1.0f, skyDefaults.groundAlbedo,
                                  "地面反射で日光と空の光を反射する割合（空の延長では下半球の明るさ）。\n"
                                  "大気の多重散乱にも効く。道路のマテリアルの色は変えない", "%.2f");
                ui::EndPropertyTable();
            }
            ui::SectionHeader("環境光");
            if (ui::BeginPropertyTable("atmosphericEnvironmentRows", 150.0f)) {
                ui::PropertyFloat("スカイライト強度", &m_renderer.SkylightIntensity(), 0.0f, 8.0f,
                                  renderer::PreviewRenderer::kDefaultSkylightIntensity,
                                  "空と地面反射から届く環境光（IBL）の倍率。1 は大気の計算そのまま。\n"
                                  "上げると陰側や環境の映り込みが明るくなる。太陽の直射光・背景の空・露出は変えない", "%.2f");
                ui::EndPropertyTable();
            }
        }

        ui::SectionHeader("露出");
        renderer::ExposureSettings& exposure = m_renderer.Exposure();
        if (ui::BeginPropertyTable("exposureRows")) {
            static const char* const kExposureModes[] = {"自動", "EV を直接指定", "物理カメラ"};
            int exposureMode = exposure.automatic ? 0 : exposure.useManualEv ? 1 : 2;
            if (ui::PropertyCombo("方式", &exposureMode, kExposureModes, IM_ARRAYSIZE(kExposureModes),
                                  kDefaultExposure.automatic ? 0 : kDefaultExposure.useManualEv ? 1 : 2,
                                  "自動は画面の輝度ヒストグラムから EV100 を測り、露出補正を足す。\n"
                                  "EV を直接指定は値をそのまま使い、物理カメラは絞り / シャッター / ISO から EV100 を求める")) {
                exposure.automatic = exposureMode == 0;
                exposure.useManualEv = exposureMode == 1;
            }
            if (exposure.automatic) {
                ui::PropertyFloat("露出補正", &exposure.compensation, -5.0f, 5.0f, kDefaultExposure.compensation,
                                  "測った EV100 に足す値。正で暗く、負で明るくなる", "%.2f EV");
                ui::PropertyFloat("EV の下限", &exposure.minEv100, -10.0f, 20.0f, kDefaultExposure.minEv100,
                                  "夜景で露出が上がりすぎないように止める値", "%.1f");
                ui::PropertyFloat("EV の上限", &exposure.maxEv100, -10.0f, 20.0f, kDefaultExposure.maxEv100,
                                  "日中で露出が下がりすぎないように止める値", "%.1f");
                ui::PropertyFloat("順応の速さ", &exposure.adaptationSpeed, 0.1f, 20.0f, kDefaultExposure.adaptationSpeed,
                                  "1 秒あたりの追従率。大きいほど明るさの変化にすぐ追従する", "%.1f /s",
                                  ImGuiSliderFlags_Logarithmic);
                ui::PropertyValue("測光 EV100", exposure.autoValid ? "%.2f" : "測定中", exposure.autoEv100);
            } else if (exposure.useManualEv) {
                ui::PropertyFloat("EV100", &exposure.manualEv100, -6.0f, 20.0f,
                                  kDefaultExposure.manualEv100, nullptr, "%.2f");
            } else {
                // **編集はカメラの節の「F 値」1 か所だけ。** ここは EV の内訳を
                // 読むための表示に留める（絞りはレンズの値で、ボケにも効くため）。
                ui::PropertyValue("絞り", "F%.1f（カメラ）", exposure.aperture);

                float shutterDenominator = 1.0f / exposure.shutterSpeed;
                if (ui::PropertyFloat("シャッター", &shutterDenominator, 1.0f, 4000.0f,
                                      1.0f / kDefaultExposure.shutterSpeed,
                                      "秒の逆数。大きいほど暗くなる", "1/%.0f 秒",
                                      ImGuiSliderFlags_Logarithmic)) {
                    exposure.shutterSpeed = 1.0f / shutterDenominator;
                }
                ui::PropertyFloat("ISO", &exposure.iso, 50.0f, 6400.0f, kDefaultExposure.iso,
                                  "感度。大きいほど明るくなる", "%.0f",
                                  ImGuiSliderFlags_Logarithmic);
            }
            ui::PropertyValue("EV100", "%.2f  (exposure %.3e)", exposure.Ev100(),
                              exposure.Exposure());
            ui::EndPropertyTable();
        }

        // **環境そのもの（何を空にするか）は天球パネルが持つ。**
        // ここに残すのは、天球ではなく見え方に属する設定だけ。
        ui::SectionHeader(atmospheric ? "背景" : "環境 (IBL)");
        if (ui::BeginPropertyTable("iblRows")) {
            const renderer::SkyAsset* activeSky = m_skyLibrary.Active();
            ui::PropertyValue("作業用IBL", "%s", (activeSky != nullptr) ? activeSky->name.c_str() : "-");
            ui::PropertyValue("環境", "%s", m_renderer.GetEnvironment().SourceName().c_str());
            ui::PropertyValue("equirect", "%u x %u", m_renderer.GetEnvironment().EquirectWidth(),
                              m_renderer.GetEnvironment().EquirectHeight());
            ui::PropertyBool("背景を表示", &m_renderer.ShowSkybox(),
                             renderer::kPreviewDefaults.showSkybox,
                             "オフにすると背景色だけになる。IBL の寄与は残る");
            ImGui::BeginDisabled(!m_renderer.ShowSkybox());
            ui::PropertyBool("背景をぼかす", &m_renderer.SkyboxBlur(),
                             renderer::kPreviewDefaults.skyboxBlur,
                             "背景だけを柔らかくする。素材を見比べるときに、"
                             "背景の細部が目移りの原因にならないようにする。"
                             "IBL の寄与と陰影は変わらない");
            ImGui::EndDisabled();
            ui::EndPropertyTable();
        }
        ui::HintText(atmospheric ? "作業用IBL（天球）は「表示環境」を作業用IBLにしたときに使う。天球パネルで切り替えられる"
                                 : "空の切り替えと輝度は「天球」パネルで設定する");

        ui::SectionHeader("トーンマップ");
        if (ui::BeginPropertyTable("tonemapRows")) {
            static const char* const kTonemapLabels[] = {"なし", "Reinhard", "ACES"};
            int tonemap = static_cast<int>(m_renderer.Tonemap());
            if (ui::PropertyCombo("方式", &tonemap, kTonemapLabels, IM_ARRAYSIZE(kTonemapLabels),
                                  static_cast<int>(renderer::kPreviewDefaults.tonemap))) {
                m_renderer.Tonemap() = static_cast<renderer::TonemapMode>(tonemap);
            }
            ui::EndPropertyTable();
        }
    }
    ImGui::End();
}

}  // namespace tg
