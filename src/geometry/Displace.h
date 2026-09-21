#pragma once
#include "geometry/Mesh.h"
#include <functional>
#include <stop_token>
#include <string>

namespace rock::geometry {
inline constexpr size_t kMaxDetailTriangles = 1000000;
struct SubdivideSettings { int levels = 1; bool operator==(const SubdivideSettings&) const = default; };
struct DisplaceSettings {
    float amount = .05f, midpoint = .5f;
    bool operator==(const DisplaceSettings&) const = default;
};
using DetailProgress = std::function<void(int)>;
using HeightSample = std::function<float(Vec3 position, Vec3 normal, Mesh::Uv uv)>;
Mesh SubdivideMesh(const Mesh&, const SubdivideSettings&, std::string&, std::stop_token = {}, DetailProgress = {});
Mesh DisplaceMesh(const Mesh&, const DisplaceSettings&, const HeightSample&, std::string&, std::stop_token = {}, DetailProgress = {});
}
