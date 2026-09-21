// スタック不要のBVH。escapeは部分木の直後、triangleが~0uなら内部ノード。
struct Triangle { float3 a; float3 b; float3 c; float2 uv0; float2 uv1; float2 uv2; };
struct Node { float3 lo; uint escape; float3 hi; uint faceIndex; };
struct Constants {
    uint triangles; uint nodes; uint output; uint uvRoot;
    uint width; uint height; uint x; uint y;
    float distance; float strength; float bias; uint samples;
};
ConstantBuffer<Constants> g : register(b0);
float Cross2(float2 a, float2 b) { return a.x*b.y-a.y*b.x; }
bool BoxHit(Node n, float3 p, float3 d) {
    float near = 0, far = g.distance;
    [unroll] for (uint axis=0; axis<3; ++axis) {
        if (abs(d[axis]) < 1e-15) {
            if (p[axis]<n.lo[axis] || p[axis]>n.hi[axis]) return false;
        } else {
            float a=(n.lo[axis]-p[axis])/d[axis], b=(n.hi[axis]-p[axis])/d[axis];
            near=max(near,min(a,b)); far=min(far,max(a,b));
            if (near>far) return false;
        }
    }
    return true;
}
bool Hit(float3 p, float3 d, uint ignored) {
    StructuredBuffer<Node> nodes=ResourceDescriptorHeap[g.nodes];
    StructuredBuffer<Triangle> triangles=ResourceDescriptorHeap[g.triangles];
    uint end=nodes[0].escape;
    for (uint i=0; i<end;) {
        Node n=nodes[i];
        if (!BoxHit(n,p,d)) { i=n.escape; continue; }
        ++i;
        if (n.faceIndex==~0u || n.faceIndex==ignored) continue;
        Triangle t=triangles[n.faceIndex];
        float3 e1=t.b-t.a, e2=t.c-t.a, q=cross(d,e2);
        float det=dot(e1,q);
        if (abs(det)<1e-7*sqrt(dot(e1,e1)*dot(e2,e2))) continue;
        float3 v=p-t.a;
        float u=dot(v,q)/det;
        if (u<0 || u>1) continue;
        q=cross(v,e1);
        float w=dot(d,q)/det;
        if (w<0 || u+w>1) continue;
        float ray=dot(e2,q)/det;
        if (ray>0 && ray<g.distance) return true;
    }
    return false;
}
[numthreads(8,8,1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    uint2 pixel=id.xy+uint2(g.x,g.y);
    if (pixel.x>=g.width || pixel.y>=g.height) return;
    StructuredBuffer<Node> nodes=ResourceDescriptorHeap[g.nodes];
    StructuredBuffer<Triangle> triangles=ResourceDescriptorHeap[g.triangles];
    RWTexture2D<float4> output=ResourceDescriptorHeap[g.output];
    float2 uv=(float2(pixel)+.5)/float2(g.width,g.height);
    uint face=~0u;
    float2 bary=0;
    uint end=nodes[g.uvRoot].escape;
    for (uint i=g.uvRoot; i<end;) {
        Node n=nodes[i];
        if (any(uv<n.lo.xy) || any(uv>n.hi.xy)) { i=n.escape; continue; }
        ++i;
        if (n.faceIndex==~0u) continue;
        Triangle t=triangles[n.faceIndex];
        float area=Cross2(t.uv1-t.uv0,t.uv2-t.uv0);
        if (abs(area)<1e-16) continue;
        float2 b=float2(Cross2(uv-t.uv0,t.uv2-t.uv0),Cross2(t.uv1-t.uv0,uv-t.uv0))/area;
        // UV共有辺はCPUの参照実装と同じく元の面番号が大きい側を採用する。
        if (all(b>=0) && b.x+b.y<=1 && (face==~0u || n.faceIndex>face)) { face=n.faceIndex; bary=b; }
    }
    float value=1;
    if (face!=~0u && g.strength>0) {
        Triangle t=triangles[face];
        float3 n=normalize(cross(t.b-t.a,t.c-t.a));
        float3 tangent=normalize(cross(abs(n.y)<.9 ? float3(0,1,0) : float3(1,0,0),n));
        float3 bitangent=cross(n,tangent);
        float3 p=t.a+(t.b-t.a)*bary.x+(t.c-t.a)*bary.y+n*g.bias;
        uint occluded=0;
        for (uint sampleIndex=0; sampleIndex<g.samples; ++sampleIndex) {
            float r=sqrt((sampleIndex+.5)/g.samples), phi=sampleIndex*2.399963229728653;
            float3 d=tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*sqrt(1-r*r);
            occluded+=Hit(p,d,face);
        }
        value=1-g.strength*float(occluded)/g.samples;
    }
    output[pixel]=float4(value,value,value,1);
}
