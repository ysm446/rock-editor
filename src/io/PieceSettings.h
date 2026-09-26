#pragma once
#include "graph/NodeGraph.h"
#include <nlohmann/json.hpp>
#include <charconv>
namespace rock::io {
inline nlohmann::json WritePiecePose(const geometry::PiecePose &s) {
    return {{"position", s.position}, {"rotation", s.rotation}, {"scale", s.scale}};
}
inline nlohmann::json WritePieceSettings(const graph::Node &node) {
    using namespace geometry;
    if (auto *s = std::get_if<LayeredBoxesSettings>(&node.settings))
        return {{"count",s->count},{"size",s->size},{"rotation",s->rotation},{"position",s->position},{"gap",s->gap},
                {"thicknessVariation",s->thicknessVariation},{"sizeVariation",s->sizeVariation},
                {"offset",s->offset},{"seed",s->seed}};
    if (auto *s = std::get_if<ScatterSettings>(&node.settings))
        return {{"count", s->count}, {"seed", s->seed}, {"version", s->version}, {"planar",s->planar}};
    if (auto *s = std::get_if<VoronoiSettings>(&node.settings))
        return {{"rotation", s->rotation}, {"stretch", s->stretch}, {"version", s->version}};
    if (auto *s = std::get_if<PieceFilterSettings>(&node.settings))
        return {{"keep", s->keep}};
    if (auto *s = std::get_if<PieceSelectSettings>(&node.settings))
        return {{"mode", int(s->mode)},
                {"outerFaces", s->outerFaces},
                {"seed", s->seed},
                {"minimum", s->minimum},
                {"maximum", s->maximum},
                {"minVolume", s->minVolume},
                {"maxVolume", s->maxVolume},
                {"fraction", s->fraction},
                {"invert", s->invert},
                {"layer",s->layer},
                {"rimLayers",s->rimLayers}, {"rimSide",s->rimSide}, {"rimFalloff",s->rimFalloff},
                {"peelNoise",s->peelNoise}, {"protectCore",s->protectCore},
                {"producer", s->producer},
                {"generation", std::to_string(s->generation)},
                {"ids", s->ids}};
    if (auto *s = std::get_if<PieceTransformSettings>(&node.settings)) {
        nlohmann::json out = {{"pose", WritePiecePose(s->pose)},
                              {"individual", s->individual},
                              {"producer", s->producer},
                              {"generation", std::to_string(s->generation)},
                              {"overrides", nlohmann::json::array()}};
        for (const auto &item : s->overrides)
            out["overrides"].push_back({{"id", item.id}, {"pose", WritePiecePose(item.pose)}});
        return out;
    }
    return nlohmann::json::object();
}
inline void ReadPieceSettings(graph::Node &node, const nlohmann::json &value) {
    using namespace geometry;
    switch (node.kind) {
    case graph::NodeKind::LayeredBoxes:
        node.settings = LayeredBoxesSettings{};
        break;
    case graph::NodeKind::ScatterPoints:
        node.settings = ScatterSettings{};
        break;
    case graph::NodeKind::VoronoiFracture:
        node.settings = VoronoiSettings{};
        break;
    case graph::NodeKind::PieceSelect:
        node.settings = PieceSelectSettings{};
        break;
    case graph::NodeKind::PieceFilter:
        node.settings = PieceFilterSettings{};
        break;
    case graph::NodeKind::PieceTransform:
        node.settings = PieceTransformSettings{};
        break;
    default:
        node.settings = std::monostate{};
        break;
    }
    const auto read = [&](const auto &json, const char *key, auto &target) {
        auto it = json.find(key);
        if (it != json.end()) {
            try {
                target = it->template get<std::decay_t<decltype(target)>>();
            } catch (const nlohmann::json::exception &) {
            }
        }
    };
    const auto generation = [&](uint64_t &v) {
        std::string text;
        read(value, "generation", text);
        auto result = std::from_chars(text.data(), text.data() + text.size(), v);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            v = 0;
    };
    const auto pose = [&](const auto &json, PiecePose &p) {
        read(json, "position", p.position);
        read(json, "rotation", p.rotation);
        read(json, "scale", p.scale);
    };
    if (auto *s = std::get_if<LayeredBoxesSettings>(&node.settings)) {
        read(value,"count",s->count); read(value,"size",s->size); read(value,"rotation",s->rotation);
        read(value,"position",s->position); read(value,"gap",s->gap); read(value,"thicknessVariation",s->thicknessVariation);
        read(value,"sizeVariation",s->sizeVariation); read(value,"offset",s->offset); read(value,"seed",s->seed);
    }
    if (auto *s = std::get_if<ScatterSettings>(&node.settings)) {
        read(value, "count", s->count);
        read(value, "seed", s->seed);
        read(value, "version", s->version);
        read(value, "planar", s->planar);
    }
    if (auto *s = std::get_if<VoronoiSettings>(&node.settings)) {
        read(value, "rotation", s->rotation);
        read(value, "stretch", s->stretch);
        read(value, "version", s->version);
    }
    if (auto *s = std::get_if<PieceFilterSettings>(&node.settings))
        read(value, "keep", s->keep);
    if (auto *s = std::get_if<PieceSelectSettings>(&node.settings)) {
        int mode = int(s->mode);
        read(value, "mode", mode);
        if (mode >= 0 && mode <= 6)
            s->mode = PieceSelectMode(mode);
        read(value, "outerFaces", s->outerFaces);
        read(value, "seed", s->seed);
        read(value, "minimum", s->minimum);
        read(value, "maximum", s->maximum);
        read(value, "minVolume", s->minVolume);
        read(value, "maxVolume", s->maxVolume);
        read(value, "fraction", s->fraction);
        read(value, "invert", s->invert);
        read(value, "layer",s->layer);
        read(value, "rimLayers",s->rimLayers);
        read(value, "rimSide",s->rimSide);
        read(value, "rimFalloff",s->rimFalloff);
        read(value, "peelNoise",s->peelNoise);
        read(value, "protectCore",s->protectCore);
        read(value, "producer", s->producer);
        generation(s->generation);
        read(value, "ids", s->ids);
    }
    if (auto *s = std::get_if<PieceTransformSettings>(&node.settings)) {
        if (value.contains("pose"))
            pose(value["pose"], s->pose);
        read(value, "individual", s->individual);
        read(value, "producer", s->producer);
        generation(s->generation);
        if (value.contains("overrides") && value["overrides"].is_array())
            for (const auto &item : value["overrides"]) {
                PieceOverride p;
                read(item, "id", p.id);
                if (item.contains("pose"))
                    pose(item["pose"], p.pose);
                s->overrides.push_back(p);
            }
    }
}
} // namespace rock::io
