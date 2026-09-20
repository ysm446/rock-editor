#pragma once
#include <vector>
#include "crack/CrackPatch.h"
namespace rock::crack {
// 有限パッチ列のルール。実切断は行わない。
struct JointSetSettings {
    std::array<float, 3> center{0, 0, 0};
    std::array<float, 3> rotationDegrees{0, 0, 0};
    float spacing = 0.6f;
    float spacingVariance = 0.15f;  // 各中心のずれ / 間隔。0～0.49。
    float angleVariance = 5;        // ローカル U/V 軸ごとの傾き上限（度）。
    float offset = 0;
    int count = 3;
    int seed = 0;
    float extentU = 1.2f, extentV = 1.2f;
    float depth = 1.6f, persistence = 0.6f, aperture = 0.02f;
    bool showGuide = true;
};
// 失敗時は出力を空にする。同じ設定は順序も含め再現する。
bool BuildJointSet(const JointSetSettings& settings, std::vector<CrackPatch>& patches, std::string& error);
}  // namespace rock::crack
