// 深度から復元した位置と法線、出来上がった色、受ける側のアルベドによる表示専用の照り返し（1 回の反射）。
// 周りの画素のうち、こちらを向いて光っている面の色を集め、受ける側の拡散の素材色を掛けて足す。
// 背景（空）と画面外は光源にしない（空の光は環境光が受け持つ）。テクスチャには焼き込まない。
struct Constants {
    uint sourceIndex, depthIndex, albedoIndex, outputIndex, width, height;
    float nearZ, farZ, tanHalfFov, radius, strength;
};
ConstantBuffer<Constants> g : register(b0);
float ViewZ(float d) { return g.nearZ*g.farZ / max(g.farZ-d*(g.farZ-g.nearZ), 1e-6); }
float3 Position(int2 p) {
    Texture2D<float> depth = ResourceDescriptorHeap[g.depthIndex];
    p = clamp(p, int2(0,0), int2(g.width-1,g.height-1));
    float z = ViewZ(depth[p]);
    float2 uv = (float2(p)+0.5)/float2(g.width,g.height);
    return float3((uv.x*2-1)*g.tanHalfFov*g.width/g.height*z,
                  (1-uv.y*2)*g.tanHalfFov*z, -z);
}
// 隣の画素との差から法線を作る（ScreenSpaceAo.hlsl と同じ）。輪郭をまたがない側の差を使う。
float3 Normal(int2 p, float3 center) {
    float3 left=center-Position(p-int2(1,0)), right=Position(p+int2(1,0))-center;
    float3 up=Position(p-int2(0,1))-center, down=center-Position(p+int2(0,1));
    float3 dx=abs(left.z)<abs(right.z)?left:right;
    float3 dy=abs(up.z)<abs(down.z)?up:down;
    float3 normal=cross(dx,dy);
    normal*=rsqrt(max(dot(normal,normal),1e-20));
    if(dot(normal,-center)<0) normal=-normal;
    return normal;
}
[numthreads(8,8,1)]
void CsMain(uint3 tid : SV_DispatchThreadID) {
    if(tid.x>=g.width || tid.y>=g.height) return;
    int2 p = tid.xy;
    Texture2D<float4> source = ResourceDescriptorHeap[g.sourceIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[g.depthIndex];
    Texture2D<float4> albedoMap = ResourceDescriptorHeap[g.albedoIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g.outputIndex];
    float4 color = source[p];
    float3 albedo = albedoMap[p].rgb;
    // 背景と、照り返しを受けない画素（モデル・ガイド線など、アルベドを書かないもの）はそのまま。
    if(depth[p]>=1.0 || g.strength<=0 || all(albedo<=0)) {output[p]=color; return;}
    float3 center=Position(p);
    float3 normal=Normal(p, center);
    float pixels=min(g.radius*g.height/(2*g.tanHalfFov*max(-center.z,g.nearZ)),192.0);
    float3 gathered=0;
    // 固定サンプルでフレーム間のちらつきを防ぐ。画素ごとに螺旋の向きを回して、縞を細かいざらつきにする。
    const float rotation=frac(52.9829189*frac(dot(float2(p),float2(0.06711056,0.00583715))))*6.2831853;
    const int kSamples=32;
    [loop] for(int i=0;i<kSamples;++i) {
        float angle=i*2.39996323+rotation;
        float2 offset=float2(cos(angle),sin(angle))*pixels*sqrt((i+0.5)/kSamples);
        int2 q=p+int2(round(offset));
        if(any(q<0)||q.x>=g.width||q.y>=g.height||all(q==p)) continue;
        float sampleDepth=depth[q];
        if(sampleDepth>=1.0) continue;
        float3 position=Position(q);
        float3 toSample=position-center;
        float distance2=dot(toSample,toSample);
        if(distance2<1e-8) continue;
        float3 direction=toSample*rsqrt(distance2);
        // 受ける側から見て前にあり、光る側がこちらを向いている面だけ。
        float receive=dot(normal,direction);
        if(receive<=0.05) continue;
        // 光る面の裏側（こちらを向いていない面）からは来ない。向きの重みは受ける側だけで付ける
        // （画面の円盤から均等に拾った点は、光る面の傾きの分だけすでに疎らになっている）。
        if(dot(Normal(q, position),-direction)<=0) continue;
        // 探す距離の外は弱める（遠い面の照り返しは小さい）。
        float falloff=saturate(1-distance2/(g.radius*g.radius*4));
        gathered+=source[q].rgb*receive*falloff;
    }
    // 半球の余弦重みの平均として扱い、サンプル数で割る。狭い隙間（半分を向かいの面が占める）で
    // 「向かいの面の明るさ × 素材色 × 0.5」ほどになるように合わせた表示用の近似。
    float3 indirect=albedo*gathered*(4.0/kSamples)*g.strength;
    output[p]=float4(min(color.rgb+indirect,60000.0),color.a);
}
