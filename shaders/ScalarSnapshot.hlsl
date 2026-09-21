struct Constants { uint inputIndex; uint outputIndex; uint width; uint height; };
ConstantBuffer<Constants> g : register(b0);
[numthreads(8,8,1)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    if(id.x>=g.width || id.y>=g.height) return;
    Texture2D<float4> inputMap=ResourceDescriptorHeap[g.inputIndex];
    RWTexture2D<float> outputMap=ResourceDescriptorHeap[g.outputIndex];
    outputMap[id.xy]=inputMap.Load(int3(id.xy,0)).r;
}
