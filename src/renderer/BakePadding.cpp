#include "renderer/BakePadding.h"
#include <algorithm>
namespace rock::renderer {
void DilateBakePixels(LdrImage &image, int padding) {
    // 元の被覆画素を起点に幅優先で広げる。島の内側を上書きしない。
    const int width = static_cast<int>(image.width), height = static_cast<int>(image.height);
    std::vector<int> frontier, next;
    for (int i = 0; i < width * height; ++i)
        if (image.pixels[size_t(i) * 4 + 3])
            frontier.push_back(i);
    for (int step = 0; step < padding && !frontier.empty(); ++step) {
        next.clear();
        for (int p : frontier) {
            const int x = p % width, y = p / width;
            const int neighbors[4] = {x > 0 ? p - 1 : -1, x + 1 < width ? p + 1 : -1, y > 0 ? p - width : -1,
                                      y + 1 < height ? p + width : -1};
            for (int q : neighbors)
                if (q >= 0 && !image.pixels[size_t(q) * 4 + 3]) {
                    std::copy_n(image.pixels.data() + size_t(p) * 4, 4, image.pixels.data() + size_t(q) * 4);
                    next.push_back(q);
                }
        }
        frontier.swap(next);
    }
}
} // namespace rock::renderer
