"""研究ページ（docs/research/rocks.md）の画像を、岩のレシピから撮り直す。

    python tools/rock_research_shots.py              # examples/ai-recipes/ の全レシピ
    python tools/rock_research_shots.py schist slate # 名前を指定して一部だけ

- 1 つのレシピを 4 方向から撮って 2×2 にまとめ（tools/rock_shot.py --views 4）、幅 1200 px の JPEG にして
  docs/research/images/<名前>.jpg に保存する。リポジトリに入れるので、大きさを抑えている。
- 岩を改善したら、そのレシピだけ撮り直して、研究ページの履歴に追記する。
- Pillow が要る。アプリ（build/bin/Release/rock_editor.exe）を先にビルドしておく。
"""
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RECIPES = REPO / 'examples' / 'ai-recipes'
IMAGES = REPO / 'docs' / 'research' / 'images'
WIDTH = 1200


def main() -> int:
    try:
        from PIL import Image
    except ImportError:
        print('Pillow が要ります（pip install pillow）', file=sys.stderr)
        return 2
    names = sys.argv[1:] or sorted(p.stem for p in RECIPES.glob('*.rockgraph'))
    IMAGES.mkdir(parents=True, exist_ok=True)
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
            out = IMAGES / f'{name}.jpg'
            image.resize((WIDTH, height), Image.LANCZOS).save(out, quality=85, optimize=True)
            print(out.relative_to(REPO))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
