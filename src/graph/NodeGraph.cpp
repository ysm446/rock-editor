#include "graph/NodeGraph.h"

#include "compositor/MaterialStack.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

namespace tg::graph {
namespace {

// --- 定義テーブル ---------------------------------------------------------
// ノードの種類・保存名・表示名・ピン構成。CreateNode() がここからピンを作る。

// **ノードの名前とピンのラベルは英語で書く。**
// ノードグラフを持つツール（Substance / Houdini / Gaea など）はどれも英語表記で、
// 素材やノードの呼び名もその語彙で流通している。説明文だけ日本語にする。
// 材質（Surface）のピン。入力は無く、Result を Road / Shoulder / Decal などの Material へ繋ぐ。
// 旧地形の Base / Mask 入力は撤去した（旧ファイルのリンクはピンが無いので読み込み時に捨てる）。
constexpr std::array<PinDefinition, 1> kLayerNodePins = {{
    {PinKind::Output, ValueType::Material, "Result"},
}};

// パスのピン。Surface に Road の RoadSurface を繋ぐと、そのパスは面の座標（横位置 × 実距離）で
// 保持され、道路を変形しても面に貼り付いたまま追従する（デカールの経路）。繋がなければ実寸 XYZ。
// 旧地形パスの Base（どの地形に沿うか）はこのピンの前身で、旧ファイルのリンクは型が違うので捨てる。
constexpr std::array<PinDefinition, 2> kPathPins = {{
    {PinKind::Input, ValueType::Mesh, "Surface"},
    {PinKind::Output, ValueType::Path, "Path"},
}};
constexpr std::array<PinDefinition, 3> kDecalPins = {{
    {PinKind::Input, ValueType::Mesh, "RoadSurface"},
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
}};
// 路肩のピン。Path には Road の Left / Right か、別の路肩の Outer を繋ぐ。
// Outer は外側の境界（実寸 Path）で、次の路肩や縁石へ渡す。
// 材質スロットとマスクは Road と同じ並び。
constexpr std::array<PinDefinition, 10> kShoulderPins = {{
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Input, ValueType::Material, "Material"},
    {PinKind::Input, ValueType::Material, "Material 2"},
    {PinKind::Input, ValueType::Material, "Material 3"},
    {PinKind::Input, ValueType::Material, "Material 4"},
    {PinKind::Input, ValueType::RoadMask, "Mask 2"},
    {PinKind::Input, ValueType::RoadMask, "Mask 3"},
    {PinKind::Input, ValueType::RoadMask, "Mask 4"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
    {PinKind::Output, ValueType::Path, "Outer"},
}};
// ひび割れのピン。Decal と同じく RoadSurface を受けて RoadSurface を返す。材質はプロパティで指定する。
constexpr std::array<PinDefinition, 2> kCrackPins = {{
    {PinKind::Input, ValueType::Mesh, "RoadSurface"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
}};
// モデルの系統のピン。どれも Model 型だけを受け渡す（道路の Mesh とは繋がらない）。
constexpr std::array<PinDefinition, 1> kModelPins = {{
    {PinKind::Output, ValueType::Model, "Model"},
}};
constexpr std::array<PinDefinition, 2> kTransformPins = {{
    {PinKind::Input, ValueType::Model, "Model"},
    {PinKind::Output, ValueType::Model, "Model"},
}};

// Merge のピン。入力は可変で、繋ぐたびに空きが 1 本増える（NormalizeVariablePins）。
// 入力は道路のメッシュとモデルのどちらも受ける。出力の型は入力から決まる（EffectiveOutputType）。
constexpr std::array<PinDefinition, 2> kMergePins = {{
    {PinKind::Input, ValueType::Any, "Input 1"},
    {PinKind::Output, ValueType::Any, "Output"},
}};

// 材質はスロット 1〜4。スロット 2〜4 は道路マスク（Mask 2〜4）で被覆する。
constexpr std::array<PinDefinition, 11> kRoadPins = {{
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Input, ValueType::Material, "Material"},
    {PinKind::Input, ValueType::Material, "Material 2"},
    {PinKind::Input, ValueType::Material, "Material 3"},
    {PinKind::Input, ValueType::Material, "Material 4"},
    {PinKind::Input, ValueType::RoadMask, "Mask 2"},
    {PinKind::Input, ValueType::RoadMask, "Mask 3"},
    {PinKind::Input, ValueType::RoadMask, "Mask 4"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
    {PinKind::Output, ValueType::Path, "Left"},
    {PinKind::Output, ValueType::Path, "Right"},
}};
constexpr std::array<PinDefinition, 1> kRoadMaskPins = {{
    {PinKind::Output, ValueType::RoadMask, "Mask"},
}};
// 道路のメッシュ・モデル・それらをまとめた Merge のどれでも受ける。
constexpr std::array<PinDefinition, 1> kMeshOutputPins = {{
    {PinKind::Input, ValueType::Any, "Mesh"},
}};
constexpr std::array<PinDefinition, 2> kRoadMarkingPins = {{
    {PinKind::Input, ValueType::Mesh, "RoadSurface"},
    {PinKind::Output, ValueType::Mesh, "RoadSurface"},
}};

constexpr std::array<NodeDefinition, 12> kNodeDefinitions = {{
    {NodeKind::Road, "road", "Road", kRoadPins},
    {NodeKind::RoadMask, "roadMask", "Road Mask", kRoadMaskPins},
    {NodeKind::Decal, "decal", "Decal", kDecalPins},
    {NodeKind::Shoulder, "shoulder", "Shoulder", kShoulderPins},
    {NodeKind::Merge, "merge", "Merge", kMergePins},
    {NodeKind::Crack, "crack", "Crack", kCrackPins},
    {NodeKind::Model, "model", "Model", kModelPins},
    {NodeKind::Transform, "transform", "Transform", kTransformPins},
    {NodeKind::RoadMarking, "roadMarking", "Lane Marking", kRoadMarkingPins},
    {NodeKind::MeshOutput, "meshOutput", "Mesh Output", kMeshOutputPins},
    {NodeKind::Surface, "surface", "Surface", kLayerNodePins},
    {NodeKind::Path, "path", "Path", kPathPins},
}};

}  // namespace

std::span<const NodeDefinition> NodeDefinitions() {
    return kNodeDefinitions;
}

const NodeDefinition* FindNodeDefinition(NodeKind kind) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.kind == kind) {
            return &definition;
        }
    }
    return nullptr;
}

const NodeDefinition* FindNodeDefinitionByName(std::string_view name) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

bool IsLayerNodeKind(NodeKind kind) {
    return kind == NodeKind::Surface;
}

bool IsMeshNodeKind(NodeKind kind) {
    return kind == NodeKind::Road || kind == NodeKind::RoadMarking || kind == NodeKind::Decal ||
           kind == NodeKind::Shoulder || kind == NodeKind::Merge || kind == NodeKind::Crack;
}

bool IsPreviewableNodeKind(NodeKind kind) {
    // 道路メッシュのノードはそのノードまでの鎖、モデルの系統のノードはその枝のモデルだけを出す。
    return IsMeshNodeKind(kind) || IsModelNodeKind(kind);
}

bool IsModelNodeKind(NodeKind kind) {
    return kind == NodeKind::Model || kind == NodeKind::Transform;
}

bool IsVariableInputNodeKind(NodeKind kind) {
    return kind == NodeKind::Merge;
}

// --- NodeGraph ------------------------------------------------------------

NodeGraph NodeGraph::CreateDefault() {
    NodeGraph graph;
    const GraphId baseId = graph.CreateNode(NodeKind::Surface);
    if (Node* base = graph.FindMutableNode(baseId)) {
        compositor::MaterialLayer layer = compositor::MaterialStack::MakeBaseLayer();
        base->settings = LayerNodeSettings{std::move(layer)};
        base->posX = 60.0f;
        base->posY = 120.0f;
        base->positionValid = true;
    }
    return graph;
}

const Pin* NodeGraph::FindPin(GraphId pinId) const {
    for (const Node& node : m_nodes) {
        for (const Pin& pin : node.inputs) {
            if (pin.id == pinId) {
                return &pin;
            }
        }
        for (const Pin& pin : node.outputs) {
            if (pin.id == pinId) {
                return &pin;
            }
        }
    }
    return nullptr;
}

const Node* NodeGraph::FindNode(GraphId nodeId) const {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [nodeId](const Node& node) { return node.id == nodeId; });
    return it == m_nodes.end() ? nullptr : &*it;
}

Node* NodeGraph::FindMutableNode(GraphId nodeId) {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [nodeId](const Node& node) { return node.id == nodeId; });
    return it == m_nodes.end() ? nullptr : &*it;
}

const Node* NodeGraph::FindUpstreamNodeForPin(GraphId inputPinId) const {
    for (const Link& link : m_links) {
        if (link.endPin != inputPinId) {
            continue;
        }
        if (const Pin* startPin = FindPin(link.startPin)) {
            return FindNode(startPin->nodeId);
        }
    }
    return nullptr;
}

// producer の出力から下流を辿り、target に届くか。循環チェックに使う。
bool NodeGraph::ReachesDownstream(GraphId fromNodeId, GraphId targetNodeId) const {
    std::unordered_set<GraphId> visited;
    std::vector<GraphId> stack{fromNodeId};
    while (!stack.empty()) {
        const GraphId current = stack.back();
        stack.pop_back();
        if (current == targetNodeId) {
            return true;
        }
        if (!visited.insert(current).second) {
            continue;
        }
        const Node* node = FindNode(current);
        if (node == nullptr) {
            continue;
        }
        for (const Pin& output : node->outputs) {
            for (const Link& link : m_links) {
                if (link.startPin != output.id) {
                    continue;
                }
                if (const Pin* endPin = FindPin(link.endPin)) {
                    stack.push_back(endPin->nodeId);
                }
            }
        }
    }
    return false;
}

bool NodeGraph::CanCreateLink(GraphId startPin, GraphId endPin) const {
    if (startPin == 0 || endPin == 0 || startPin == endPin) {
        return false;
    }
    const Pin* start = FindPin(startPin);
    const Pin* end = FindPin(endPin);
    if (start == nullptr || end == nullptr || start->nodeId == end->nodeId || start->kind == end->kind) {
        return false;
    }
    // 出力側 → 入力側へ揃えてから型と循環を見る。
    if (start->kind == PinKind::Input) {
        std::swap(start, end);
    }
    const ValueType startType = EffectiveOutputType(start->id);
    if (!TypesCompatible(startType, end->valueType)) {
        return false;
    }
    // end（消費側）の下流に start（生産側）がいたら、この接続で輪ができる。
    if (ReachesDownstream(end->nodeId, start->nodeId)) {
        return false;
    }
    // Merge へ繋ぐと出力の型が変わることがある。下流がその型を受けられなければ繋がない
    // （モデルだけを Transform へ渡している Merge に道路を足す、など）。
    if (const Node* consumer = FindNode(end->nodeId); consumer != nullptr && consumer->kind == NodeKind::Merge) {
        const ValueType next = MergeTypeWith(*consumer, end->id, startType, 0);
        if (!DownstreamAccepts(*consumer, next, 0)) return false;
    }
    return true;
}

bool NodeGraph::TypesCompatible(ValueType output, ValueType input) {
    const auto scene = [](ValueType type) {
        return type == ValueType::Mesh || type == ValueType::Model || type == ValueType::Any;
    };
    if (input == ValueType::Any) return scene(output);
    if (output == ValueType::Any) return input == ValueType::Mesh || input == ValueType::Model;
    return output == input;
}

GraphId NodeGraph::FindUpstreamPin(GraphId inputPinId) const {
    for (const Link& link : m_links)
        if (link.endPin == inputPinId) return link.startPin;
    return 0;
}

ValueType NodeGraph::MergeTypeWith(const Node& merge, GraphId replacedPin, ValueType replacement, int depth) const {
    bool any = false, modelOnly = true;
    for (const Pin& pin : merge.inputs) {
        ValueType type = ValueType::Any;
        if (pin.id == replacedPin) {
            type = replacement;
        } else if (const GraphId upstream = FindUpstreamPin(pin.id); upstream != 0 && depth < 64) {
            const Pin* source = FindPin(upstream);
            const Node* producer = source ? FindNode(source->nodeId) : nullptr;
            type = (producer && producer->kind == NodeKind::Merge) ? MergeTypeWith(*producer, 0, ValueType::Any, depth + 1)
                                                                  : (source ? source->valueType : ValueType::Any);
        } else {
            continue;
        }
        if (type == ValueType::Any) continue;
        any = true;
        if (type != ValueType::Model) modelOnly = false;
    }
    return !any ? ValueType::Any : (modelOnly ? ValueType::Model : ValueType::Mesh);
}

ValueType NodeGraph::EffectiveOutputType(GraphId outputPin) const {
    const Pin* pin = FindPin(outputPin);
    if (pin == nullptr) return ValueType::Any;
    const Node* node = FindNode(pin->nodeId);
    if (node != nullptr && node->kind == NodeKind::Merge) return MergeTypeWith(*node, 0, ValueType::Any, 0);
    return pin->valueType;
}

bool NodeGraph::DownstreamAccepts(const Node& node, ValueType newType, int depth) const {
    if (depth > 64) return false;
    for (const Pin& output : node.outputs) {
        for (const Link& link : m_links) {
            if (link.startPin != output.id) continue;
            const Pin* target = FindPin(link.endPin);
            if (target == nullptr) continue;
            if (!TypesCompatible(newType, target->valueType)) return false;
            // 下流の Merge は、その出力の型も変わり得る。
            const Node* consumer = FindNode(target->nodeId);
            if (consumer != nullptr && consumer->kind == NodeKind::Merge &&
                !DownstreamAccepts(*consumer, MergeTypeWith(*consumer, target->id, newType, depth + 1), depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

bool NodeGraph::CreateLink(GraphId startPin, GraphId endPin) {
    if (!CanCreateLink(startPin, endPin)) {
        return false;
    }
    const Pin* start = FindPin(startPin);
    if (start != nullptr && start->kind == PinKind::Input) {
        std::swap(startPin, endPin);
    }
    // 入力ピンは 1 本だけ。既にある接続は置き換える。
    std::erase_if(m_links, [endPin](const Link& link) { return link.endPin == endPin; });
    m_links.push_back({AllocateGraphId(), startPin, endPin});
    NormalizeVariablePins();
    MarkDirty();
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId) {
    const size_t oldSize = m_links.size();
    std::erase_if(m_links, [linkId](const Link& link) { return link.id == linkId; });
    if (m_links.size() == oldSize) {
        return false;
    }
    NormalizeVariablePins();
    MarkDirty();
    return true;
}

void NodeGraph::NormalizeVariablePins() {
    for (Node& node : m_nodes) {
        if (!IsVariableInputNodeKind(node.kind)) continue;
        // 繋がっている入力を順に残し、末尾に空きを 1 本だけ置く。ラベルは並びで振り直す。
        std::vector<Pin> connected;
        Pin spare;
        for (const Pin& pin : node.inputs) {
            const bool linked = std::any_of(m_links.begin(), m_links.end(),
                                            [&](const Link& link) { return link.endPin == pin.id; });
            if (linked) connected.push_back(pin);
            else if (spare.id == 0) spare = pin;
        }
        // 未接続ピンも UI と保存データが参照するため、既存の ID を維持する。
        if (spare.id == 0) spare.id = AllocateGraphId();
        spare.nodeId = node.id;
        spare.kind = PinKind::Input;
        spare.valueType = ValueType::Any;
        connected.push_back(std::move(spare));
        for (size_t i = 0; i < connected.size(); ++i) {
            connected[i].valueType = ValueType::Any;
            connected[i].label = "Input " + std::to_string(i + 1);
        }
        node.inputs = std::move(connected);
    }
    // 入力を外して型が合わなくなった下流のリンクを外す（Merge の出力が Mesh ↔ Model に変わったとき）。
    // 外すとさらに下流の型が変わり得るので、変化が無くなるまで繰り返す。
    for (bool removed = true; removed;) {
        removed = false;
        for (auto it = m_links.begin(); it != m_links.end(); ++it) {
            const Pin* end = FindPin(it->endPin);
            if (end != nullptr && !TypesCompatible(EffectiveOutputType(it->startPin), end->valueType)) {
                m_links.erase(it);
                removed = true;
                break;
            }
        }
    }
    // Merge の出力ピンの型は表示（ピンの色）用に今の型を入れておく。
    for (Node& node : m_nodes) {
        if (node.kind != NodeKind::Merge) continue;
        for (Pin& output : node.outputs) output.valueType = MergeTypeWith(node, 0, ValueType::Any, 0);
    }
}

GraphId NodeGraph::CreateNode(NodeKind kind) {
    const NodeDefinition* definition = FindNodeDefinition(kind);
    if (definition == nullptr) {
        return 0;
    }
    Node node;
    node.id = AllocateGraphId();
    node.kind = kind;
    if (IsLayerNodeKind(kind)) {
        node.settings = LayerNodeSettings{};
    } else if (kind == NodeKind::Road) {
        node.settings = RoadNodeSettings{};
    } else if (kind == NodeKind::RoadMarking) {
        node.settings = RoadMarkingNodeSettings{};
    } else if (kind == NodeKind::RoadMask) {
        node.settings = RoadMaskNodeSettings{};
    } else if (kind == NodeKind::Decal) {
        node.settings = DecalNodeSettings{};
    } else if (kind == NodeKind::Shoulder) {
        node.settings = ShoulderNodeSettings{};
    } else if (kind == NodeKind::Merge) {
        node.settings = MergeNodeSettings{};
    } else if (kind == NodeKind::Crack) {
        node.settings = CrackNodeSettings{};
    } else if (kind == NodeKind::Model) {
        node.settings = ModelNodeSettings{};
    } else if (kind == NodeKind::Transform) {
        node.settings = TransformNodeSettings{};
    } else if (kind == NodeKind::Path) {
        node.settings = PathNodeSettings{};
    } else {
        // Mesh Output は設定を持たない。
        node.settings = std::monostate{};
    }
    for (const PinDefinition& pin : definition->pins) {
        Pin created;
        created.id = AllocateGraphId();
        created.nodeId = node.id;
        created.kind = pin.kind;
        created.valueType = pin.valueType;
        created.label = pin.label;
        if (pin.kind == PinKind::Input) {
            node.inputs.push_back(std::move(created));
        } else {
            node.outputs.push_back(std::move(created));
        }
    }
    const GraphId nodeId = node.id;
    m_nodes.push_back(std::move(node));
    MarkDirty();
    return nodeId;
}

bool NodeGraph::DeleteNode(GraphId nodeId) {
    const Node* node = FindNode(nodeId);
    if (node == nullptr) {
        return false;
    }
    std::vector<GraphId> pinIds;
    pinIds.reserve(node->inputs.size() + node->outputs.size());
    for (const Pin& pin : node->inputs) {
        pinIds.push_back(pin.id);
    }
    for (const Pin& pin : node->outputs) {
        pinIds.push_back(pin.id);
    }
    std::erase_if(m_links, [&pinIds](const Link& link) {
        return std::find(pinIds.begin(), pinIds.end(), link.startPin) != pinIds.end() ||
               std::find(pinIds.begin(), pinIds.end(), link.endPin) != pinIds.end();
    });
    std::erase_if(m_nodes, [nodeId](const Node& candidate) { return candidate.id == nodeId; });
    NormalizeVariablePins();
    MarkDirty();
    return true;
}

void NodeGraph::Replace(std::vector<Node> nodes, std::vector<Link> links) {
    m_nodes = std::move(nodes);
    m_links.clear();
    // 編集時と同じ DAG・入力 1 本・型の規則を復元時にも適用する。循環や入力の重複を作るリンクは捨てる。
    // Merge の出力の型は入力のリンクで決まるので、ファイルの並びに依らないよう、採れるものが無くなるまで繰り返す。
    std::vector<Link> pending = std::move(links);
    for (bool progress = true; progress && !pending.empty();) {
        progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            const Pin* start = FindPin(it->startPin);
            const Pin* end = FindPin(it->endPin);
            if (start == nullptr || end == nullptr || start->kind != PinKind::Output || end->kind != PinKind::Input ||
                FindUpstreamNodeForPin(it->endPin) != nullptr) {
                it = pending.erase(it);
                continue;
            }
            if (!CanCreateLink(it->startPin, it->endPin)) {
                ++it;
                continue;
            }
            m_links.push_back(*it);
            it = pending.erase(it);
            progress = true;
        }
    }
    RebuildNextGraphId();
    NormalizeVariablePins();
    MarkDirty();
}

void NodeGraph::RebuildNextGraphId() {
    GraphId maxId = 0;
    for (const Node& node : m_nodes) {
        maxId = std::max(maxId, node.id);
        for (const Pin& pin : node.inputs) {
            maxId = std::max(maxId, pin.id);
        }
        for (const Pin& pin : node.outputs) {
            maxId = std::max(maxId, pin.id);
        }
    }
    for (const Link& link : m_links) {
        maxId = std::max(maxId, link.id);
    }
    m_nextGraphId = maxId + 1;
}

CompiledGraph NodeGraph::CompileSurface(const Node* surface) const {
    CompiledGraph compiled;
    const auto* settings =
        (surface != nullptr) ? std::get_if<LayerNodeSettings>(&surface->settings) : nullptr;
    // 有効な Surface が無ければ、評価器はどの出力テクスチャにも書かず、
    // 直前の評価結果がそのまま見えてしまう。変位 0 の中立平面へ戻す。
    if (settings == nullptr || !settings->layer.enabled) {
        compiled.layers.push_back(compositor::MaterialStack::MakeBaseLayer());
        compiled.layerSources.push_back(0);
        return compiled;
    }
    compiled.layers.push_back(settings->layer);
    compiled.layerSources.push_back(surface->id);
    return compiled;
}

CompiledGraph NodeGraph::CompileLayers() const {
    // 既定は下地 1 枚。
    return CompileSurface(nullptr);
}

CompiledGraph NodeGraph::CompileLayersTo(GraphId nodeId, GraphId /*outputPin*/) const {
    const Node* node = FindNode(nodeId);
    if (node == nullptr || !IsLayerNodeKind(node->kind)) {
        // Path やメッシュのノードはレイヤー列を持たない。下地 1 枚（中立平面）になる。
        return CompileLayers();
    }
    return CompileSurface(node);
}

}  // namespace tg::graph
