#pragma once
#include "core/ImageIo.h"
namespace rock::renderer {
// 被覆画素（アルファが 0 でない画素）の色を、外側へ padding 画素だけ伸ばす。島の内側は変えない。
void DilateBakePixels(LdrImage &image, int padding);
// UV の島が無い部分を全て、最も近い島の縁の色で埋める（エッジパディング）。結果は全面が不透明になる。
// 空きが黒や透明のままだと、縮小表示やミップマップで島の縁へその色がにじむ。
// 被覆画素が1つも無い画像は変えない。
void FillBakeBackground(LdrImage &image);
}
