#include "TestSupport.h"
#include "geometry/ShapeMask.h"
#include "geometry/Mesh.h"
#include "geometry/Volume.h"
#include "graph/RockEvaluator.h"

#include <cmath>
#include <numbers>
#include <string>

using namespace rock::tests;
using namespace rock;

namespace {
// 閉じた箱のメッシュに、天面だけ UV の全面を割り当て、他の面は小さな島にする。
// 天面は X 軸まわりに傾けて、+Z 側が低くなる（水は +Z へ流れ下る）。UV の v は Z に沿う。
geometry::Mesh TiltedBox(float tiltDegrees) {
    geometry::Mesh mesh = geometry::MakeBox({2, .4f, 2});
    mesh.cornerUvs.assign(mesh.triangles.size(), {{{0, 0}, {.001f, 0}, {0, .001f}}});
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        const auto& t = mesh.triangles[f];
        const auto a = mesh.positions[t[0]], b = mesh.positions[t[1]], c = mesh.positions[t[2]];
        const bool top = a.y > 0 && b.y > 0 && c.y > 0;
        if (!top) continue;
        for (int i = 0; i < 3; ++i) {
            const auto p = mesh.positions[t[size_t(i)]];
            mesh.cornerUvs[f][size_t(i)] = {(p.x + 1) * .5f, (p.z + 1) * .5f};
        }
    }
    const float c = std::cos(tiltDegrees * std::numbers::pi_v<float> / 180), s = std::sin(tiltDegrees * std::numbers::pi_v<float> / 180);
    for (auto& p : mesh.positions) {
        const float y = p.y * c - p.z * s, z = p.y * s + p.z * c;
        p.y = y;
        p.z = z;
    }
    mesh.uvWidth = 256;
    mesh.uvHeight = 256;
    return mesh;
}
}  // namespace

void RunFlowMaskTests() {
    Section("Flow Mask");
    std::string error;
    // 天面を 30° 傾けた箱。+Z（UV の v が大きい側）へ水が流れ下る。
    const auto mesh = TiltedBox(-30);
    geometry::FlowMaskSettings settings;
    settings.resolution = 256;
    settings.volumeResolution = 64;
    settings.length = .5f;
    settings.width = .02f;
    settings.sharpness = 1;
    const auto image = geometry::FlowMask(mesh, settings, nullptr, error);
    Check(error.empty() && image.width == 256 && image.height == 256, "傾いた箱の天面に流れのマスクを作れる");
    // 傾斜の向き: X 軸まわりに -30° 回すと天面の +Z 側が高くなる（y' = y cos − z sin、sin(−30°) < 0）。
    // 水は −Z（UV の v が小さい側）へ流れ下る。
    const float vLow = .15f, vHigh = .85f;
    const float low = image.Sample(.5f, vLow), high = image.Sample(.5f, vHigh);
    Check(low > high + .2f, "流れが集まる斜面の低い側が白く、高い側（出発点）は黒い");
    Check(image.Sample(.5f, .5f) > high, "斜面の中ほどは上端より流れが多い");

    const auto again = geometry::FlowMask(mesh, settings, nullptr, error);
    Check(error.empty() && again.pixels == image.pixels, "並列計算でも結果は再現可能");
    {
        // Volume 入力: メッシュを格子にしたものを渡すと、変換した場合と同じ結果になる。
        geometry::VolumeSettings volumeSettings;
        volumeSettings.resolution = settings.volumeResolution;
        const auto grid = geometry::MeshToVolume(mesh, volumeSettings, error);
        const auto fromVolume = geometry::FlowMask(mesh, settings, &grid, error);
        Check(error.empty() && fromVolume.pixels == image.pixels, "Volume 入力を渡すとその格子で筋を追う（変換と同じ結果）");
    }
    geometry::FlowMaskSettings sharper = settings;
    sharper.sharpness = 4;
    const auto focused = geometry::FlowMask(mesh, sharper, nullptr, error);
    Check(error.empty() && focused.Sample(.5f, .5f) <= image.Sample(.5f, .5f), "集中を上げると中ほどの値が下がる");
    geometry::FlowMaskSettings inverted = settings;
    inverted.invert = true;
    Check(geometry::FlowMask(mesh, inverted, nullptr, error).pixels == image.pixels, "反転は使う側で掛ける");

    for (auto bad : {[](auto s) { s.resolution = 100; return s; }(settings), [](auto s) { s.volumeResolution = 8; return s; }(settings),
                     [](auto s) { s.length = 0; return s; }(settings), [](auto s) { s.width = 1; return s; }(settings),
                     [](auto s) { s.sharpness = 0; return s; }(settings)})
        Check(geometry::FlowMask(mesh, bad, nullptr, error).pixels.empty() && !error.empty(), "不正な設定を拒否する");
    Check(geometry::FlowMask(geometry::MakeBox({1, 1, 1}), settings, nullptr, error).pixels.empty() && !error.empty(), "UV なしを診断する");
    std::stop_source stop;
    stop.request_stop();
    Check(geometry::FlowMask(mesh, settings, nullptr, error, stop.get_token()).pixels.empty() && !error.empty(), "キャンセルできる");

    // グラフ: Base Shape → UV Unwrap → Flow Mask。
    graph::NodeGraph g;
    const auto base = g.CreateNode(graph::NodeKind::BaseRock), uv = g.CreateNode(graph::NodeKind::UvUnwrap),
               node = g.CreateNode(graph::NodeKind::FlowMask);
    std::get<geometry::UvUnwrapSettings>(g.FindMutableNode(uv)->settings).resolution = 128;
    auto& nodeSettings = std::get<geometry::FlowMaskSettings>(g.FindMutableNode(node)->settings);
    nodeSettings.resolution = 128;
    nodeSettings.volumeResolution = 32;
    g.CreateLink(g.FindNode(base)->outputs[0].id, g.FindNode(uv)->inputs[0].id);
    Check(!graph::EvaluateRocks(g, node).error.empty(), "未接続なら診断する");
    g.CreateLink(g.FindNode(uv)->outputs[0].id, g.FindNode(node)->inputs[0].id);
    graph::RockEvaluationCache cache;
    const auto first = graph::EvaluateRocks(g, node, &cache);
    Check(first.error.empty() && first.rocks.size() == 1 && first.rocks[0].previewMask, "グラフで評価できる");
    std::get<geometry::FlowMaskSettings>(g.FindMutableNode(node)->settings).sharpness = 6;
    Check(graph::EvaluateRocks(g, node, &cache).rocks[0].previewMask != first.rocks[0].previewMask, "設定の変更で作り直す");
}
