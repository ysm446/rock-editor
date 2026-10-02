"""岩グラフの Rock Asset をアプリで焼く（LLM が対話せずに岩アセットを作るための道具）。

    python tools/rock_bake.py <graph.rockgraph> [--node <id>] [--config Release]

- アプリを起動してグラフを読み、Rock Asset ノードを「岩アセットを焼く」と同じ手順で焼く（上流の Material Bake が
  古ければ先にベイクする）。付属フォルダ `<graph>.rockgraph.bake/`（asset.json、lodN.rockmesh、テクスチャ）ができる。
- --node を省くと、グラフの中の Rock Asset ノード（1 つ）を自動で選ぶ。複数あれば --node で指定する。
- 焼いた後にグラフを同じパスへ保存して終了する（内容は読み込んだものと同じ。サムネイルも更新される）。
- GPU とウィンドウを使い、数十秒かかることがある。アプリの設定（最近使ったファイルなど）は一時フォルダへ隔離する。
- 山グラフ（.mountaingraph）の Rock ノードは、この付属フォルダを読む。焼き直したら山グラフ側で「読み直す」か、
  グラフを編集すると読み直す。
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def find_root(graph: Path) -> Path:
    for folder in graph.resolve().parents:
        if (folder / 'project.reproj').exists():
            return folder
    return graph.resolve().parent


def find_rock_asset_node(graph: Path) -> int:
    with open(graph, encoding='utf-8') as handle:
        document = json.load(handle)
    nodes = [node for node in document.get('graph', {}).get('nodes', []) if node.get('kind') == 'rockAsset']
    if not nodes:
        raise SystemExit('Rock Asset ノードがありません。Mesh Output の前に Rock Asset を置いてください')
    if len(nodes) > 1:
        raise SystemExit('Rock Asset ノードが複数あります。--node で指定してください: ' + ', '.join(str(n['id']) for n in nodes))
    return int(nodes[0]['id'])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('graph', type=Path)
    parser.add_argument('--node', type=int, help='焼く Rock Asset ノードの id')
    parser.add_argument('--config', default='Release')
    args = parser.parse_args()

    exe = REPO / 'build' / 'bin' / args.config / 'rock_editor.exe'
    if not exe.exists():
        print(f'アプリがありません: {exe}（cmake --build build --config {args.config} --target rock_editor）', file=sys.stderr)
        return 2
    graph = args.graph.resolve()
    if not graph.exists():
        print(f'グラフがありません: {graph}', file=sys.stderr)
        return 2
    node = args.node if args.node else find_rock_asset_node(graph)
    folder = graph.with_name(graph.name + '.bake')
    manifest = folder / 'asset.json'
    before = manifest.stat().st_mtime if manifest.exists() else None
    command = [str(exe), '--root', str(find_root(graph)), '--project', str(graph), '--bake-asset', str(node),
               '--save-project', str(graph)]
    env = dict(os.environ)
    env['LOCALAPPDATA'] = str(Path(tempfile.gettempdir()) / 'rock_shot_settings')
    result = subprocess.run(command, cwd=REPO, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    for line in result.stderr.splitlines():
        if '[error]' in line or '[warn]' in line:
            print(line, file=sys.stderr)
    if result.returncode != 0 or not manifest.exists() or (before is not None and manifest.stat().st_mtime <= before):
        print(f'焼けませんでした（終了コード {result.returncode}）。--screenshot-ui で画面のエラーを確かめてください', file=sys.stderr)
        return 1
    with open(manifest, encoding='utf-8') as handle:
        data = json.load(handle)
    lods = data.get('lods', [])
    print(json.dumps({'folder': str(folder), 'lods': len(lods), 'textured': data.get('textured', False),
                      'minimum': data.get('minimum'), 'maximum': data.get('maximum')}, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main())
