#ifndef ROCK_ATMOSPHERE_INTEGRATION
#define ROCK_ATMOSPHERE_INTEGRATION
// 大気の光路積分の補助。rock-editor から移植（月と夜空の補助は持ってこない）。
// C++ からも読めるようにしてある（PreviewRenderer::EffectiveLight が太陽の透過率を CPU で求める）。
#ifdef __cplusplus
#include <cmath>
namespace rock::renderer {
#define ROCK_ATM_INLINE inline
#else
#define ROCK_ATM_INLINE
#endif
// (1-exp(-tau))/tau。薄い区間での桁落ちを避け、tau=0 の極限は 1。
ROCK_ATM_INLINE float AtmosphereSegmentWeight(float tau) {
    if(tau<0.001f) return 1.0f-tau*0.5f+tau*tau/6.0f;
#ifdef __cplusplus
    return -std::expm1(-tau)/tau;
#else
    return (1-exp(-tau))/tau;
#endif
}
// 光路の分割。地表近くへ点を集中させる。
ROCK_ATM_INLINE float AtmosphereRayFraction(float u) { return u*u; }
#ifdef __cplusplus
}
#endif
#undef ROCK_ATM_INLINE
#endif
