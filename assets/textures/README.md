# UVチェッカー

`uv_checker.png` は2048×2048、16×16マスの表示用画像。Houdiniの参考画像に合わせ、明るいマスは行名A〜P、暗いマスは列番号1〜16を表示する。先頭行は `A 2 A 4 … A 16`、次行は `1 B 3 B … 15 B`。アプリはこのPNGをそのまま読み、0〜1のUVに1枚配置する。

`python tools/generate_uv_checker.py` で再生成できる。PillowとWindowsのBahnschriftフォントを使う。フォントファイルは同梱せず、画像に描画した結果のみを配布する。別環境では `--font` でフォントを指定する。

CMakeのビルド後に実行ファイル横の `assets/textures/` へコピーする。チェッカーは表示専用で、シーンの材質・ベイク画像には含めない。
