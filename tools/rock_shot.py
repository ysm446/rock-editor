"""岩グラフをアプリで開いて、ビューポートを PNG に撮る（LLM が見た目を確かめるための道具）。

    python tools/rock_shot.py <graph.rockgraph> <out.png> [--node <id>] [--ui] [--config Release|Debug]

- グラフの評価が終わるのを待ってから撮る。カメラは形全体が入るように引く（--frame-all）。
- プロジェクトのルートはグラフのファイルから上へ project.reproj を探して決める。無ければグラフのフォルダ。
- アプリの設定（最近使ったファイルなど）は一時フォルダへ隔離し、手元の設定を変えない。
- --node で途中のノードを撮る（アプリのプレビューと同じ）。--ui は UI ごと撮る（評価エラーが画面に出る）。
- 形の数値は先に `rock_cli eval` で確かめる。撮影は GPU とウィンドウを使い、数秒〜十数秒かかる。
"""
import argparse
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('graph', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--node', type=int, help='途中のノードを撮る')
    parser.add_argument('--ui', action='store_true', help='UI ごと撮る')
    parser.add_argument('--config', default='Release')
    parser.add_argument('--frame', type=int, default=30, help='最低限待つフレーム数（評価の完了も待つ）')
    args = parser.parse_args()

    exe = REPO / 'build' / 'bin' / args.config / 'rock_editor.exe'
    if not exe.exists():
        print(f'アプリがありません: {exe}（cmake --build build --config {args.config} --target rock_editor）', file=sys.stderr)
        return 2
    graph = args.graph.resolve()
    out = args.out.resolve()
    command = [str(exe), '--root', str(find_root(graph)), '--project', str(graph), '--frame-all',
               '--screenshot-frame', str(args.frame), '--screenshot-ui' if args.ui else '--screenshot', str(out)]
    if args.node:
        command += ['--preview-node', str(args.node)]
    env = dict(os.environ)
    env['LOCALAPPDATA'] = str(Path(tempfile.gettempdir()) / 'rock_shot_settings')
    result = subprocess.run(command, cwd=REPO, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    errors = [line for line in result.stderr.splitlines() if '[error]' in line]
    for line in errors:
        print(line, file=sys.stderr)
    if result.returncode != 0 or not out.exists():
        print(f'撮影に失敗しました（終了コード {result.returncode}）', file=sys.stderr)
        return 1
    print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
