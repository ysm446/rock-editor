#include "rhi/Common.h"

#include "core/Log.h"

namespace rock::rhi {

bool CheckHr(HRESULT hr, const char* expr, const char* file, int line) {
    if (SUCCEEDED(hr)) {
        return true;
    }
    ROCK_LOG_ERROR("%s:%d: %s が 0x%08lX で失敗しました", file, line, expr,
                 static_cast<unsigned long>(hr));
    return false;
}

}  // namespace rock::rhi
