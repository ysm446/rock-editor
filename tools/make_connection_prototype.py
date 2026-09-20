"""既存プロジェクトの素材から P0 検証用コピーを作る。元データと素材は変更しない。"""

import argparse
import copy
import json
import math
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--asphalt", type=int, required=True, help="既存の材質 ID")
    parser.add_argument("--gravel", type=int, required=True, help="既存の材質 ID")
    parser.add_argument("--layered", action="store_true", help="道路4層＋接続先4層＋歩道の検証構成を作る")
    parser.add_argument("--length", type=float, default=24.0, help="検証道路の長さ（m、0より大きく50以下）")
    parser.add_argument("--layout-description", action="store_true", help="版18のプリセット・配置記述を追加（道路本体の開発用プレビューに対応）")
    args = parser.parse_args()
    if not math.isfinite(args.length) or not 0 < args.length <= 50:
        parser.error("長さは0より大きく50 m以下にしてください")
    if args.source.resolve() == args.output.resolve():
        parser.error("出力には元ファイルと異なるパスを指定してください")
    doc = json.loads(args.source.read_text(encoding="utf-8-sig"))
    materials = {m["id"]: m for m in doc.get("materials", [])}
    if args.asphalt not in materials or args.gravel not in materials:
        parser.error("指定した材質 ID がありません")
    doc["materials"] = [copy.deepcopy(materials[i]) for i in dict.fromkeys([args.asphalt, args.gravel])]
    used = set()
    for material in doc["materials"]:
        for slot in material.get("maps", {}).values():
            texture = slot.get("texture") if isinstance(slot, dict) else slot
            if isinstance(texture, int):
                used.add(texture)
    doc["textures"] = [t for t in doc.get("textures", []) if t["id"] in used]
    for texture in doc["textures"]:
        texture["path"] = str((args.source.parent / texture["path"]).resolve())
        if not Path(texture["path"]).is_file():
            parser.error(f"素材が見つかりません: {texture['path']}")
    # 検証画像は環境の差を避けて手続き空を使う。
    doc["skies"] = [{"name": "試作用の空", "source": "procedural", "skyLuminance": 12000,
                     "iblIntensity": 1, "procedural": {"intensity": 12000,
                     "zenithColor": [0.2, 0.36, 0.78], "horizonColor": [0.7, 0.8, 0.95],
                     "groundColor": [0.45, 0.42, 0.38]}}]
    doc["activeSky"] = 0
    doc["version"] = 16
    doc.pop("surfaceLayouts", None)
    def surface(node_id, pin, material, position):
        return {"id": node_id, "kind": "surface", "inputs": [], "outputs": [pin], "position": position,
                "layer": {"enabled": True, "material": material, "baseColor": [0.42, 0.4, 0.36],
                          "roughness": 0.8, "height": {"source": "constant", "base": 0.5, "gain": 1.0}}}
    doc["graph"] = {"nodes": [
        surface(1, 2, args.asphalt, [0, 0]), surface(3, 4, args.gravel, [0, 180]),
        surface(5, 6, None, [0, 360]),
        {"id": 7, "kind": "path", "inputs": [8], "outputs": [9], "position": [240, 0],
         "path": {"worldSpace": True, "points": [{"id": 1, "position": [0, 0, -args.length * 0.5]},
                   {"id": 2, "position": [0, 0, args.length * 0.5]}],
                  "edges": [{"id": 3, "from": 1, "to": 2, "curve": "line"}], "nextId": 4}},
        {"id": 10, "kind": "road", "inputs": list(range(11, 19)), "outputs": [19, 20, 21],
         "position": [480, 0], "road": {"width": 6, "uvRepeat": 2, "displacement": 0.015}},
        {"id": 22, "kind": "meshOutput", "inputs": [23], "outputs": [], "position": [720, 0]}],
        "links": [{"id": 24, "start": 9, "end": 11}, {"id": 25, "start": 2, "end": 12},
                  {"id": 26, "start": 19, "end": 23}]}
    gravel_node = 3
    if args.layered:
        nodes = doc["graph"]["nodes"]
        links = doc["graph"]["links"]
        gravel_node = 30
        nodes.append({"id": 30, "kind": "road", "inputs": list(range(31, 39)),
                      "outputs": [39, 40, 41], "position": [480, 420],
                      "road": {"width": 10, "uvRepeat": 2.8, "displacement": 0.08}})
        links.extend([{"id": 42, "start": 9, "end": 31}, {"id": 43, "start": 4, "end": 32}])
        next_id = 50
        for group, road_id in enumerate((10, 30)):
            road_node = next(n for n in nodes if n["id"] == road_id)
            road_node["road"].update({"layerUvRepeat": [2, 1.3, 3.1, 0.8],
                                      "layerBlendMode": [0, 0, 1, 0], "layerBlendRange": 0.12,
                                      "layerHeightGate": [0, 0, 0, 2],
                                      "layerHeightGateThreshold": [0.5, 0.5, 0.5, 0.6]})
            for slot, shape in enumerate(("wheelTracks", "lengthNoise", "edgeFalloff"), 1):
                material_id = args.asphalt if group == 0 else args.gravel
                layer = surface(next_id, next_id + 1, material_id, [0, 600 + next_id * 3])
                nodes.append(layer)
                nodes.append({"id": next_id + 2, "kind": "roadMask", "inputs": [],
                              "outputs": [next_id + 3], "position": [220, 600 + next_id * 3],
                              "roadMask": {"shape": shape, "strength": 0.65, "seed": 13 + group * 3 + slot,
                                           "noiseScale": 2.5, "edgeWidth": 0.7}})
                links.extend([{"id": next_id + 4, "start": next_id + 1, "end": road_node["inputs"][slot + 1]},
                              {"id": next_id + 5, "start": next_id + 3, "end": road_node["inputs"][slot + 4]}])
                next_id += 6
    if args.layout_description:
        doc["version"] = 18
        data = {"version": 2, "nextId": 1, "presets": [], "layouts": []}
        def allocate():
            value = data["nextId"]
            data["nextId"] += 1
            return value
        for index, name in enumerate(("新舗装", "荒れた舗装", "砂利道", "砂利路肩", "草地", "歩道")):
            points = [(0, 0), (6 if index < 3 else 2, 0)]
            if index == 5:
                points = [(0, 0), (0, 0.15), (2, 0.15)]
            preset = {"id": allocate(), "version": 1, "name": name, "role": 0 if index < 3 else 2 if index == 5 else 1,
                      "displacement": 0.015 if index < 2 else 0.08 if index < 5 else 0,
                      "section": [{"id": allocate(), "across": x, "height": y} for x, y in points],
                      "boundaries": [{"mode": 1 if index == 5 else 0, "transition": 0.5,
                                      "maxHeightAdjustment": 0.15, "preserveOutline": index == 5} for _ in range(4)],
                      "materials": [{"material": args.asphalt if index < 2 else args.gravel if index < 5 else 0,
                                     "uvRepeat": 2, "worldUv": False, "baseColor": [0.42, 0.4, 0.36], "roughness": 0.8}],
                      "parameters": [{"id": allocate(), "name": "荒れ具合", "minimum": 0, "maximum": 1, "default": 0.5}]}
            preset["layerBlendRange"] = 0.12
            base = preset["materials"][0]
            base.update({"metallic": 0, "ambientOcclusion": 1, "mask": None, "blendMode": 0,
                         "heightGate": 0, "heightGateThreshold": 0.5, "heightGateSoftness": 0.2})
            if args.layered and index < 3:
                for slot, shape in enumerate((0, 2, 1), 1):
                    layer = copy.deepcopy(base)
                    layer.update({"material": args.gravel if index < 2 else args.asphalt,
                                  "uvRepeat": (1.3, 3.1, 0.8)[slot - 1], "blendMode": 1 if slot == 2 else 0,
                                  "heightGate": 2 if slot == 3 else 0, "heightGateThreshold": 0.6,
                                  "mask": {"shape": shape, "edgeSide": 0, "laneOffset": 1.5,
                                           "trackSpacing": 1.5, "trackWidth": 0.35, "feather": 0.25,
                                           "tracksFromLanes": True, "bothLanes": True, "edgeWidth": 0.7,
                                           "noiseScale": 2.5, "threshold": 0.5, "softness": 0.2,
                                           "seed": 13 + index * 3 + slot, "breakupAmount": 0.3,
                                           "breakupScale": 3, "strength": (0.15, 0.65, 0.3)[index], "invert": False}})
                    preset["materials"].append(layer)
            data["presets"].append(preset)
        layout = {"id": allocate(), "roadNode": 10, "bands": []}
        for side, cuts in enumerate(((0, 0.24, 0.56, 1), (0, 0.34, 0.7, 1))):
            band = {"id": allocate(), "side": side, "spans": []}
            for index in range(3):
                preset = data["presets"][side * 3 + index]
                band["spans"].append({"id": allocate(), "preset": preset["id"],
                                      "start": cuts[index] * args.length, "end": cuts[index + 1] * args.length,
                                      "blendIn": min(2, (cuts[index + 1] - cuts[index]) * args.length / 2) if index else 0,
                                      "blendOut": min(2, (cuts[index + 1] - cuts[index]) * args.length / 2) if index < 2 else 0, "seed": 17,
                                      "parameters": [{"parameter": preset["parameters"][0]["id"], "start": 0.2, "end": 0.8}]})
            layout["bands"].append(band)
        data["layouts"].append(layout)
        doc["surfaceLayouts"] = data
    preview = doc.setdefault("preview", {})
    preview["camera"] = {"target": [0, 0, 0], "distance": max(16, args.length * 26 / 24),
                         "yaw": -0.55, "pitch": 0.9, "fovY": 0.785398}
    preview["depthOfField"] = {"enabled": False}
    preview["exposure"] = {"useManualEv": True, "manualEv100": 14}
    preview["materialResolution"] = 1024
    preview["tessellation"] = True
    preview["tessellationFactor"] = 16
    preview["tessellationTargetPixels"] = 8
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"作成: {args.output}\n起動オプション: --project {args.output} --connection-prototype 10 {gravel_node} 5")
    if args.layout_description:
        print(f"道路配置の確認: --project {args.output} --surface-layout-preview 10（沿道・公開値の結線は未対応）")


if __name__ == "__main__":
    main()
