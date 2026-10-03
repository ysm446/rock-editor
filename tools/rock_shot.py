"""岩グラフをアプリで開いて、ビューポートを PNG に撮る（LLM が見た目を確かめるための道具）。

    python tools/rock_shot.py <graph.rockgraph> <out.png> [--node <id>] [--views 4] [--yaw <deg>] [--pitch <deg>] [--ui]
                              [--light-azimuth <deg>] [--light-elevation <deg>] [--light-illuminance <lux>] [--exposure <EV>] [--skylight <x>] [--lighting ibl|atmospheric]

- グラフの評価が終わるのを待ってから撮る。カメラは形全体が入るように引く（--frame-all）。
- プロジェクトのルートはグラフのファイルから上へ project.reproj を探して決める。無ければグラフのフォルダ。
- アプリの設定（最近使ったファイルなど）は一時フォルダへ隔離し、手元の設定を変えない。
- --node で途中のノードを撮る（アプリのプレビューと同じ）。--ui は UI ごと撮る（評価エラーが画面に出る）。
- --views 4 は 90 度ずつ回した 4 方向を 2×2 の 1 枚にまとめる（Pillow が無ければ <out>_0.png〜_3.png に分けて保存）。
  --yaw / --pitch は 1 方向の向き（度。pitch は正で見下ろす）。
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
    parser.add_argument('--views', type=int, default=1, help='回して撮る方向の数（4 で 2×2 の 1 枚）')
    parser.add_argument('--yaw', type=float, help='水平の向き（度）')
    parser.add_argument('--pitch', type=float, help='見下ろす角度（度）')
    parser.add_argument('--light-azimuth', type=float, help='光の方位（度）。省くとカメラの少し横')
    parser.add_argument('--light-elevation', type=float, help='光の仰角（度）。昼の直射日光は 50〜65')
    parser.add_argument('--light-illuminance', type=float, help='光の照度（lux）。晴天の直射日光は 100000')
    parser.add_argument('--exposure', type=float, help='露出補正（EV）。正で暗く、負で明るく')
    parser.add_argument('--skylight', type=float, help='環境光（空の IBL）の倍率。既定 1')
    parser.add_argument('--lighting', choices=['ibl', 'atmospheric'], help='表示環境。atmospheric はシーンの空（大気散乱の太陽と空）')
    args = parser.parse_args()

    exe = REPO / 'build' / 'bin' / args.config / 'rock_editor.exe'
    if not exe.exists():
        print(f'アプリがありません: {exe}（cmake --build build --config {args.config} --target rock_editor）', file=sys.stderr)
        return 2
    graph = args.graph.resolve()
    out = args.out.resolve()
    if args.views <= 1:
        return shoot(exe, graph, out, args, args.yaw, args.pitch)
    base = 35.0 if args.yaw is None else args.yaw
    pitch = 20.0 if args.pitch is None else args.pitch
    parts = [out.with_name(f'{out.stem}_{i}{out.suffix}') for i in range(args.views)]
    for i, part in enumerate(parts):
        if shoot(exe, graph, part, args, base + 360.0 * i / args.views, pitch) != 0:
            return 1
    try:
        from PIL import Image
    except ImportError:
        print('\n'.join(str(p) for p in parts))
        return 0
    images = [Image.open(p) for p in parts]
    w, h = images[0].size
    columns = 2 if args.views == 4 else args.views
    rows = (args.views + columns - 1) // columns
    sheet = Image.new('RGB', (w * columns, h * rows))
    for i, image in enumerate(images):
        sheet.paste(image, ((i % columns) * w, (i // columns) * h))
    sheet.save(out)
    for p in parts:
        p.unlink()
    print(out)
    return 0


def shoot(exe: Path, graph: Path, out: Path, args, yaw, pitch) -> int:
    command = [str(exe), '--root', str(find_root(graph)), '--project', str(graph), '--frame-all',
               '--screenshot-frame', str(args.frame), '--screenshot-ui' if args.ui else '--screenshot', str(out)]
    if args.node:
        command += ['--preview-node', str(args.node)]
    if yaw is not None:
        # 光はカメラの少し横から当てる（既定の視点と光の角度の差に合わせる）。裏側を撮っても影で潰れない。
        command += ['--camera-yaw', str(yaw)]
        if args.light_azimuth is None:
            command += ['--light-azimuth', str(yaw + 18.0)]
    if pitch is not None:
        command += ['--camera-pitch', str(pitch)]
    if args.light_azimuth is not None:
        command += ['--light-azimuth', str(args.light_azimuth)]
    if args.light_elevation is not None:
        command += ['--light-elevation', str(args.light_elevation)]
    if args.light_illuminance is not None:
        command += ['--light-illuminance', str(args.light_illuminance)]
    if args.exposure is not None:
        command += ['--exposure', str(args.exposure)]
    if args.skylight is not None:
        command += ['--skylight', str(args.skylight)]
    if args.lighting:
        command += ['--lighting', args.lighting]
    env = dict(os.environ)
    env['LOCALAPPDATA'] = str(Path(tempfile.gettempdir()) / 'rock_shot_settings')
    if out.exists():
        out.unlink()
    result = subprocess.run(command, cwd=REPO, env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    for line in result.stderr.splitlines():
        if '[error]' in line:
            print(line, file=sys.stderr)
    if result.returncode != 0 or not out.exists():
        print(f'撮影に失敗しました（終了コード {result.returncode}）', file=sys.stderr)
        return 1
    if args.views <= 1:
        print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
