#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
namespace acz::probe {
// Portable selection policy exercised by fake-runtime fixtures and used by the
// real backend after recording the complete enumerations. No fallback policy.
inline bool d3d12_extension(const std::vector<std::string>& extensions) {
    return std::find(extensions.begin(),extensions.end(),"XR_KHR_D3D12_enable")!=extensions.end();
}
inline bool stereo_config(const std::vector<int>& configs) {
    return std::find(configs.begin(),configs.end(),2)!=configs.end(); // PRIMARY_STEREO
}
struct ViewLimits { uint32_t width,height,samples,max_width,max_height,max_samples; };
inline bool stereo_views(const std::vector<ViewLimits>& eyes) {
    if(eyes.size()!=2)return false;
    return std::all_of(eyes.begin(),eyes.end(),[](const ViewLimits& e){return e.width&&e.height&&e.samples&&
        e.width<=e.max_width&&e.height<=e.max_height&&e.samples<=e.max_samples;});
}
inline bool opaque_blend(const std::vector<int>& modes) {
    return std::find(modes.begin(),modes.end(),1)!=modes.end(); // OPAQUE only
}
struct GraphicsFacts {
    uint32_t required_low=0, selected_low=0;
    int32_t required_high=0, selected_high=0;
    uint32_t minimum=0, selected=0;
    bool adapter_found=false,device_created=false,queue_created=false,direct=false,same_device=false;
};
inline bool compatible_graphics(const GraphicsFacts& f) {
    return f.minimum!=0 && f.adapter_found&&f.device_created&&f.queue_created&&f.direct&&f.same_device&&
        f.required_low==f.selected_low&&f.required_high==f.selected_high&&f.selected>=f.minimum;
}
}
