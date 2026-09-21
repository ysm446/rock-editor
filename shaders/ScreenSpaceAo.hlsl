// 深度から復元した位置と法線による表示専用AO。背景と画面外は遮蔽物にしない。
struct Constants {
    uint sourceIndex, depthIndex, outputIndex, width, height;
    float nearZ, farZ, tanHalfFov, radius, strength;
};
ConstantBuffer<Constants> g : register(b0);
float3 Position(int2 p) {
    Texture2D<float> depth = ResourceDescriptorHeap[g.depthIndex];
    p = clamp(p, int2(0,0), int2(g.width-1,g.height-1));
    float d = depth[p];
    float z = g.nearZ*g.farZ / max(g.farZ-d*(g.farZ-g.nearZ), 1e-6);
    float2 uv = (float2(p)+0.5)/float2(g.width,g.height);
    return float3((uv.x*2-1)*g.tanHalfFov*g.width/g.height*z,
                  (1-uv.y*2)*g.tanHalfFov*z, -z);
}
[numthreads(8,8,1)]
void CsMain(uint3 tid : SV_DispatchThreadID) {
    if(tid.x>=g.width || tid.y>=g.height) return;
    int2 p = tid.xy;
    Texture2D<float4> source = ResourceDescriptorHeap[g.sourceIndex];
    Texture2D<float> depth = ResourceDescriptorHeap[g.depthIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g.outputIndex];
    float4 color = source[p];
    if(depth[p]>=1.0 || g.strength<=0) {output[p]=color; return;}
    float3 center=Position(p);
    float3 left=center-Position(p-int2(1,0)), right=Position(p+int2(1,0))-center;
    float3 up=Position(p-int2(0,1))-center, down=center-Position(p+int2(0,1));
    float3 dx=abs(left.z)<abs(right.z)?left:right;
    float3 dy=abs(up.z)<abs(down.z)?up:down;
    float3 normal=cross(dx,dy);
    normal*=rsqrt(max(dot(normal,normal),1e-20));
    if(dot(normal,-center)<0) normal=-normal;
    float pixels=min(g.radius*g.height/(2*g.tanHalfFov*max(-center.z,g.nearZ)),128.0);
    float occlusion=0;
    // 固定サンプルでフレーム間のちらつきを防ぐ。遠い面・輪郭の向こう側は距離で除外。
    [unroll] for(int i=0;i<32;++i) {
        float angle=i*2.39996323;
        float2 offset=float2(cos(angle),sin(angle))*pixels*sqrt((i+0.5)/32.0);
        int2 q=p+int2(round(offset));
        if(any(q<0)||q.x>=g.width||q.y>=g.height||all(q==p)) continue;
        if(depth[q]>=1.0) continue;
        float3 delta=Position(q)-center;
        float distance=length(delta);
        float facing=max(0,dot(normal,delta)/max(distance,1e-6)-0.06);
        occlusion+=facing*saturate(1-distance/g.radius);
    }
    float ao=saturate(1-g.strength*occlusion*(3.0/32.0));
    output[p]=float4(color.rgb*ao,color.a);
}
