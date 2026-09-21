#pragma once
#include "geometry/Mesh.h"
#include <string>
namespace rock::geometry {
struct UvUnwrapSettings {
    int resolution = 1024;
    int padding = 4;
    int quality = 1;
    bool operator==(const UvUnwrapSettings &) const = default;
};
bool HasValidUvs(const Mesh &mesh);
// 旧シーンの任意解像度を、画素数を減らさず128〜4096の2のべき乗へ揃える。
int NormalizeUvResolution(int resolution);
Mesh UnwrapMesh(const Mesh &input, const UvUnwrapSettings &settings, std::string &error);
} // namespace rock::geometry
