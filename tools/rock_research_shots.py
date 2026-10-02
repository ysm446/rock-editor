"""研究ページ（docs/research/<名前>/README.md）の画像を、岩のレシピから撮り直す。

    python tools/rock_research_shots.py                 # examples/ai-recipes/ の全レシピ
    python tools/rock_research_shots.py schist slate    # 名前を指定して一部だけ
    python tools/rock_research_shots.py --no-archive x  # 経過の画像を残さず latest.jpg だけ更新

- 1 つのレシピを 4 方向から撮って 2×2 にまとめ（tools/rock_shot.py --views 4）、幅 1200 px の JPEG にして
  docs/research/<名前>/latest.jpg に保存する（アプリの「テンプレートから作成」のサムネイルにも使う）。
  同時に docs/research/<名前>/<YYYYMMDD-HHMM>.jpg にも残し、経過が見られるようにする（前の画像と同じなら残さない）。
  リポジトリに入れるので、大きさを抑えている。
- 岩を改善したら、そのレシピだけ撮り直して、研究ページの「経過」に追記する。
- Pillow が要る。アプリ（build/bin/Release/rock_editor.exe）を先にビルドしておく。
"""
import datetime
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RECIPES = REPO / 'examples' / 'ai-recipes'
RESEARCH = REPO / 'docs' / 'research'
WIDTH = 1200


def main() -> int:
    try:
        from PIL import Image
    except ImportError:
        print('Pillow が要ります（pip install pillow）', file=sys.stderr)
        return 2
    args = sys.argv[1:]
    archive = '--no-archive' not in args
    args = [a for a in args if a != '--no-archive']
    names = args or sorted(p.stem for p in RECIPES.glob('*.rockgraph'))
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M')
    failed = []
    with tempfile.TemporaryDirectory() as temp:
        for name in names:
            graph = RECIPES / f'{name}.rockgraph'
            if not graph.exists():
                print(f'レシピがありません: {graph}', file=sys.stderr)
                failed.append(name)
                continue
            sheet = Path(temp) / f'{name}.png'
            result = subprocess.run([sys.executable, str(REPO / 'tools' / 'rock_shot.py'), str(graph), str(sheet), '--views', '4'],
                                    capture_output=True, text=True, encoding='utf-8', errors='replace')
            if result.returncode != 0 or not sheet.exists():
                print(f'撮影に失敗しました: {name}\n{result.stderr}', file=sys.stderr)
                failed.append(name)
                continue
            image = Image.open(sheet).convert('RGB')
            height = round(image.height * WIDTH / image.width)
            folder = RESEARCH / name
            folder.mkdir(parents=True, exist_ok=True)
            out = folder / 'latest.jpg'
            previous = out.read_bytes() if out.exists() else None
            image.resize((WIDTH, height), Image.LANCZOS).save(out, quality=85, optimize=True)
            print(out.relative_to(REPO))
            if archive and out.read_bytes() != previous:
                dated = folder / f'{stamp}.jpg'
                dated.write_bytes(out.read_bytes())
                print(dated.relative_to(REPO))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
