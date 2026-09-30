#pragma once

#include "graph/NodeGraph.h"

#include <span>

// ノードの設定の項目表（範囲・単位・列挙の候補・意味）。
//
// LLM などアプリの外の道具がグラフを書くためのカタログ（rock_cli catalog）と、読み込み時の範囲の診断に使う。
// 項目の場所は保存形式の JSON のパス（"volumeCrack.width" など。ノードの項目の中での位置）で指す。
//
// **正本は評価のコード（範囲外ならエラーにする処理）と保存処理のまま。** この表はそれを写したもので、
// tests/NodeParamsTests.cpp が「既定値が範囲内」「パスが保存形式に実在する」「範囲の端で評価が通り、外で
// そのノードが失敗する」を確かめて、ずれを検出する。ノードや範囲を変えたらこの表も直すこと。
// GPU・UI に依存しない。
namespace rock::graph {

enum class ParamType {
    Float,
    Int,
    Bool,
    String,
    Float3,      // [x, y, z]
    FloatArray,  // 数値の配列（Rock Asset の段ごとの値など）
    BoolArray,
    Enum,        // 文字列の列挙。options の name を書く
    IntEnum,     // 数値の列挙（旧形式）。options の value を書く
};

// 範囲外の値をどう扱うか。
enum class ParamCheck {
    Error,    // 評価でそのノードがエラーになる
    Clamp,    // 読み込みか評価で範囲へ丸める（エラーにならない）
    None,     // 確かめない（書いた値がそのまま使われる）
};

struct ParamOption {
    const char* name;     // 保存名（Enum）か、意味の短い名前（IntEnum）
    int value;            // IntEnum の値（Enum では並び順）
    const char* meaning;  // 日本語の説明
};

struct ParamDefinition {
    NodeKind kind;
    const char* path;         // ノードの項目の中での JSON のパス
    ParamType type;
    double minimum, maximum;  // 範囲（両端を含む）。範囲の無い項目は NaN
    const char* unit;         // "m"、"度"、"最長辺に対する比"、"0〜1" など。無ければ ""
    const char* label;        // UI の表示名（日本語）
    const char* meaning;      // 意味と使いどころ（日本語）
    ParamCheck check = ParamCheck::None;
    std::span<const ParamOption> options = {};
    bool internal = false;    // 内部の管理用。手で書かない（ピースの世代・ID など）
};

// ノードの種類ごとの概要。何をするノードか、LLM が組むときに知っておくべき注意。
struct NodeSummary {
    NodeKind kind;
    const char* purpose;  // 何をするか
    const char* notes;    // 落とし穴・組み方の注意（無ければ ""）
};

std::span<const ParamDefinition> NodeParams();
std::span<const NodeSummary> NodeSummaries();
const NodeSummary* FindNodeSummary(NodeKind kind);

}  // namespace rock::graph
