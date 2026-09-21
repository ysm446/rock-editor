#include "app/Application.h"
#include "geometry/BakeOcclusion.h"
#include <algorithm>
#include <cmath>

namespace rock {
// --test-gpu-ao: 実デバイス上でCPU参照値・進捗・部分実行の破棄を検証する。
bool Application::ValidateGpuAo() {
    geometry::Mesh mesh;
    mesh.positions={{-1,0,-1},{1,0,-1},{1,0,1},{-1,0,1}};
    mesh.triangles={{0,2,1},{0,3,2}};
    mesh.cornerUvs={{{{0,0},{.5f,1},{.5f,0}}},{{{0,0},{0,1},{.5f,1}}}};
    mesh.uvWidth=137; mesh.uvHeight=75; // タイル端とreadbackのrow pitchも検証。
    renderer::OcclusionBake gpu;
    std::string error;
    const auto compare=[&](float distance,int samples,float strength) {
        std::vector<uint8_t> cpu;
        if(!geometry::BakeOcclusion(mesh,distance,samples,strength,cpu,error) ||
           !gpu.Create(m_device,mesh,distance,samples,strength,error)) return false;
        float previous=0;
        while(!gpu.Complete()) {
            if(!gpu.Step(m_device,m_pipelineCache,error)) { gpu.Release(m_device); return false; }
            if(gpu.Progress()<=previous || gpu.Progress()>1) { error="AO progress is invalid"; gpu.Release(m_device); return false; }
            previous=gpu.Progress();
        }
        LdrImage image;
        const bool read=gpu.Read(m_device,image); gpu.Release(m_device);
        if(!read || image.width!=mesh.uvWidth || image.height!=mesh.uvHeight) { error="AO readback failed"; return false; }
        size_t mismatches=0; int maximum=0;
        for(size_t i=0;i<cpu.size();++i) {
            const int delta=std::abs(int(cpu[i])-int(image.pixels[i*4]));
            maximum=std::max(maximum,delta); mismatches+=delta>1;
        }
        ROCK_LOG_INFO("GPU AO comparison: distance=%.2f samples=%d strength=%.2f, max delta=%d, mismatches=%zu/%zu",distance,samples,strength,maximum,mismatches,cpu.size());
        if(mismatches) { error="GPU AO differs from CPU reference"; return false; }
        return true;
    };
    bool ok=compare(1,32,1);
    for(int i=0;i<4;++i) { auto p=mesh.positions[i]; p.y=.2f; mesh.positions.push_back(p); }
    mesh.triangles.push_back({4,5,6}); mesh.triangles.push_back({4,6,7});
    mesh.cornerUvs.push_back({{{.5f,0},{1,0},{1,1}}}); mesh.cornerUvs.push_back({{{.5f,0},{1,1},{.5f,1}}});
    ok=ok && compare(1,32,1) && compare(.1f,32,1) && compare(1,128,.5f) && compare(1,8,0);
    if(ok) {
        ok=gpu.Create(m_device,mesh,1,32,1,error) && gpu.Step(m_device,m_pipelineCache,error) && !gpu.Complete();
        gpu.Release(m_device);
        ok=ok && gpu.Progress()==0 && !gpu.Complete() && compare(1,32,1);
    }
    if(ok) ok=!gpu.Create(m_device,mesh,1,0,1,error);
    gpu.Release(m_device);
    m_device.WaitForGpu(); m_device.DrainDebugMessages();
    if(!ok) ROCK_LOG_ERROR("GPU AO validation failed: %s",error.c_str());
    else ROCK_LOG_INFO("GPU AO validation passed");
    return ok;
}
}
