// (hookshot) NVIDIA DLSS Super Resolution in place of OoT's gameplay upscale network (vk_dlss.h).
//
// OoT has no DLSS of its own: its scene is drawn at a lower size and a network of its own (the program
// fd029a317a9a4b94 at 3/4, another for each other share; one dispatch, cooperative matrices) scales it up, from the
// square root of the scene's tonemapped colour, into the frame's B10G11R11 picture (before the HUD). Here that dispatch is replaced:
//  - the scene's camera is taken from the draws into the scene's colour target: their vertex stage's uniform buffer 3
//    holds the target's size at 0x120, this frame's view-projection at 0x30 (row major, clip = M * position; the projection's w row ends -1, 0 at 0xa8). The largest such target in a frame is
//    the scene (reflections and half-size passes have the same layout). The scene's depth is that draw's depth target.
//  - shaders/dlss_prep.comp makes DLSS's inputs at the render size: the colour as the network samples it (LDR), the
//    depth, and motion vectors from the depth reprojected through the two cameras (the game has none: only the
//    camera's motion, what moves by itself ghosts). No jitter yet (the game's camera has none).
//  - DLSS (NGX, Vulkan) scales them to the output's size; shaders/dlss_out.comp squares that back to linear colour
//    into the network's output.
// Anything unexpected (no camera this frame, other sizes, DLSS not there or failing) leaves the network to run, the
// reason in the log when it changes.
//
// RX_VK_DLSS=0 / 1 / 2: the game's own, DLSS, DLAA over the Insert menu's Upscaler row (0 also leaves NGX's extensions
// off). RX_VK_DLSS_DIR=<folder>: where DLSS's runtime is looked for
// first (Windows: nvngx_dlss.dll, else in the game folder's ngx\, then %LOCALAPPDATA%\Hookshot\ngx; Linux:
// libnvidia-ngx-dlss.so.*, else in the game folder's ngx/, then $XDG_DATA_HOME/hookshot/ngx). RX_VK_DLSS_PRESET=<letter>:
// DLSS's preset (default K).
// RX_VK_DLSS_DEBUG=motion: the motion vectors shown instead of the picture. RX_VK_DLSS_LOG=1: NGX's own log in ours.
// RX_VK_DLSS_SQUARE=0: the output not squared.
//
// NVIDIA's NGX SDK is not part of Hookshot (NVIDIA's license): a local build fetches it (scripts/ngx_sdk.py) and builds with
// HOOKSHOT_NGX_SDK, which compiles this file with HOOKSHOT_NGX. Without it only the stubs at the end are here.
// NGX's core is the driver's (Windows: _nvngx.dll, which NVIDIA's loader finds in the driver's own folder; Linux:
// libnvidia-ngx.so). It is never put beside the game: a copy there would be loaded in place of the driver's.
#include "vk_dlss.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "vk_draw.h"
#include "vk_menu.h"
#include "vk_paths.h"
#include "vk_texture.h"

#ifdef HOOKSHOT_NGX
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"

#include "shaders/dlss_out_comp_spv.h"
#include "shaders/dlss_prep_comp_spv.h"
#endif

namespace mkwvk {

namespace {
int env_switch(const char* name) {   // (-1: unset)
    const char* e = getenv(name);
    return e && *e ? atoi(e) : -1;
}
}  // namespace

// (what is known of DLSS on this device: -1 not yet, else a DlssWhy. Set once the device is made and when NGX answers)
std::atomic<int> g_why{-1};

DlssWhy dlss_why() {
    if (!dlss_built()) return DLSS_NOT_BUILT;
    static const bool env_off = env_switch("RX_VK_DLSS") == 0;
    if (env_off) return DLSS_ENV_OFF;
    const int w = g_why.load(std::memory_order_relaxed);
    return w < 0 ? DLSS_OK : (DlssWhy)w;
}

int dlss_mode() {
    if (dlss_why() != DLSS_OK) return 0;
    static const int env = env_switch("RX_VK_DLSS");
    return std::clamp(env >= 0 ? env : opts.dlss, 0, 2);
}
bool dlss_wanted() { return dlss_mode() != 0; }
bool dlss_full_scale() { return dlss_mode() == 2; }

#ifdef HOOKSHOT_NGX

namespace {

// (the gameplay networks, which vk_draw.cpp's dlss_stand_in finds: the 4/3 upscale (fd029a317a9a4b94) when the 3D is drawn at
// 3/4 of the picture, the game's dynamic resolution's usual size; a pass at the same size (9dc98871eb7fc101) when it is drawn
// at full size, the Render resolution row's 100% and DLAA's; and a network of its own for each other fixed share)

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

struct Camera {
    float current[16]{};      // (its view-projection)
    float jitter[2]{};        // (the projection's offset of the picture's centre, in clip space: nx2/oot_jitter.py)
    uint32_t width = 0, height = 0;
    Texture* depth = nullptr;
    uint64_t depth_gen = 0;   // (texture_destroy_generation when taken: a depth texture destroyed since is not used)
    uint64_t frame = 0;       // (texture_frame_no when taken)
    bool valid = false;
};

struct State {
    std::mutex mu;
    bool ngx_tried = false, ngx_ok = false, failed = false;
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t feature_rw = 0, feature_rh = 0, feature_ow = 0, feature_oh = 0;   // (its largest render size, its output)
    uint32_t min_rw = 0, min_rh = 0;
    Image color, depth, motion, output;
    VkPipelineLayout prep_layout = VK_NULL_HANDLE, out_layout = VK_NULL_HANDLE;
    VkPipeline prep = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    Camera cam;               // (this frame's, since the last upscale)
    uint64_t last_frame = 0;  // (the frame DLSS last drew)
    float last_jitter[2]{};   // (its camera's jitter: the frame before's, for the next one)
    float last_vp[16]{};      // (its camera's view-projection: the frame before's)
    std::atomic<uint32_t> jitter_w{0}, jitter_h{0};   // (the scene's size while DLSS draws: the host's jitter's scale)
    std::unordered_map<VkImage, VkImageView> depth_views;
    uint64_t depth_views_gen = 0;
    const char* reason = nullptr;
    int reason_repeats = 0;
    bool told_first = false;
    struct Dead { uint64_t frame; std::function<void()> free; };
    std::vector<Dead> dead;   // (what work in flight may still use: freed a few frames on)
};
State& st() {
    static State s;
    return s;
}

bool fail(const char* why) {   // (the network runs: why, when it changes and every 600th time)
    State& s = st();
    if (why != s.reason || ++s.reason_repeats % 600 == 0) logf("vk: NVIDIA DLSS: the game's upscale runs (%s)", why);
    if (why != s.reason) s.reason = why, s.reason_repeats = 0;
    return false;
}

void later(std::function<void()> f) { st().dead.push_back({texture_frame_no(), std::move(f)}); }
void free_dead(bool all) {
    State& s = st();
    const uint64_t now = texture_frame_no();
    for (size_t i = 0; i < s.dead.size();) {
        if (all || now - s.dead[i].frame > 4) {
            s.dead[i].free();
            s.dead[i] = std::move(s.dead.back());
            s.dead.pop_back();
        } else {
            i++;
        }
    }
}

void drop_image(Image& im) {
    if (!im.image) return;
    const Image old = im;
    later([old] {
        vkDestroyImageView(dev.device, old.view, nullptr);
        destroy_image(old.image, old.memory);
    });
    im = {};
}

bool make_image(VkCommandBuffer cmd, Image& im, VkFormat format, uint32_t w, uint32_t h) {
    if (im.image && im.format == format && im.width == w && im.height == h) return true;
    drop_image(im);
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D, ic.format = format, ic.extent = {w, h, 1}, ic.mipLevels = 1, ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT, ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!create_image(ic, im.image, im.memory)) return im = {}, false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = im.image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev.device, &vc, nullptr, &im.view) != VK_SUCCESS) {
        destroy_image(im.image, im.memory);
        return im = {}, false;
    }
    im.format = format, im.width = w, im.height = h;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image, b.subresourceRange = vc.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    return true;
}

NVSDK_NGX_Resource_VK resource(const Image& im, bool write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, im.format,
                                                  im.width, im.height, write);
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string m(message ? message : "");
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    logf("ngx: %s", m.c_str());
}

std::string game_dir() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1);
}
/// A path for NGX, which takes wide strings. Windows: the narrow paths here are the ANSI code page's (GetModuleFileNameA, getenv),
/// so they are converted from it, and a game folder with letters outside ASCII is still the right folder
std::wstring wide(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

#ifdef _WIN32
constexpr const char* kRuntime = "nvngx_dlss.dll";   // (DLSS's runtime, as scripts/ngx_sdk.py fetches it)
#else
constexpr const char* kRuntime = "libnvidia-ngx-dlss.so";
#endif

/// NGX on this device, once: false when DLSS cannot run here (the reason logged)
bool ngx_init() {
    State& s = st();
    if (s.ngx_tried) return s.ngx_ok;
    s.ngx_tried = true;
    // where DLSS's runtime (kRuntime) is looked for (NGX takes the newest it finds), and its own files go
    std::vector<std::string> dirs;
    if (const char* e = getenv("RX_VK_DLSS_DIR"); e && *e) dirs.push_back(e);
    dirs.push_back(game_dir() + "ngx");
#ifdef _WIN32
    if (const char* l = getenv("LOCALAPPDATA"); l && *l) dirs.push_back(std::string(l) + "\\Hookshot\\ngx");
#else
    if (const char* x = getenv("XDG_DATA_HOME"); x && *x) dirs.push_back(std::string(x) + "/hookshot/ngx");
    else if (const char* h = getenv("HOME")) dirs.push_back(std::string(h) + "/.local/share/hookshot/ngx");
#endif
    std::vector<std::wstring> wdirs;
    for (const auto& d : dirs) wdirs.push_back(wide(d));
    std::vector<const wchar_t*> paths;
    for (const auto& d : wdirs) paths.push_back(d.c_str());
    const std::string data = game_dir() + "ngx";
    std::filesystem::create_directories(data);   // (NGX's logs and its own files: the game folder's, not /usr/share)
    const std::wstring wdata = wide(data);
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths.data();
    info.PathListInfo.Length = (unsigned)paths.size();
    static const bool verbose = env_switch("RX_VK_DLSS_LOG") > 0;
    info.LoggingInfo.LoggingCallback = verbose ? ngx_log : nullptr;
    info.LoggingInfo.MinimumLoggingLevel = verbose ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    // (NGX also writes its own log files into the data folder; with RX_VK_DLSS_LOG=1 its lines go to ours instead)
    info.LoggingInfo.DisableOtherLoggingSinks = verbose;
    // (a project id of Hookshot's own: NGX wants one with the custom engine type, any GUID)
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID("7b2f5d0c-6a1e-4c39-9f84-3d5e2a1b0c96", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                              "1.0", wdata.c_str(), dev.instance, dev.physical, dev.device,
                                                              instance_proc(), vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: NVIDIA DLSS: NGX did not start (%#x): not an NVIDIA GPU, or its driver has no NGX", (unsigned)r);
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.caps)) || !s.caps) {
        logf("vk: NVIDIA DLSS: NGX has no capabilities for this device");
        g_why = DLSS_NEEDS_RTX;
        return false;
    }
    int available = 0, needs_driver = 0;
    unsigned major = 0, minor = 0;
    s.caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    s.caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    s.caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
    s.caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
    if (!available) {
        std::string where;
        for (const auto& d : dirs) where += (where.empty() ? "" : ", ") + d;
        // (for the menu: a runtime found in none of the folders is the likely reason; with one there, the GPU (a GTX))
        int init = 0;
        s.caps->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init);
        bool runtime = false;
        std::error_code ec;
        for (const auto& d : dirs)
            for (const auto& f : std::filesystem::directory_iterator(d, ec))
                runtime |= f.path().filename().string().rfind(kRuntime, 0) == 0;
        if (needs_driver) logf("vk: NVIDIA DLSS: needs NVIDIA's driver %u.%u or newer", major, minor);
        else if (!runtime) logf("vk: NVIDIA DLSS: not available: no %s in %s (scripts/ngx_sdk.py fetches it)", kRuntime, where.c_str());
        else logf("vk: NVIDIA DLSS: not available on this GPU (%#x): DLSS needs an NVIDIA RTX GPU", (unsigned)init);
        g_why = needs_driver ? DLSS_NEEDS_DRIVER : !runtime ? DLSS_NO_RUNTIME : DLSS_NEEDS_RTX;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&s.params)) || !s.params) return false;
    logf("vk: NVIDIA DLSS: NGX started (DLSS from %s)", dirs.size() > 1 ? "the folders searched" : dirs[0].c_str());
    s.ngx_ok = true;
    g_why = DLSS_OK;
    return true;
}

bool make_pipelines() {
    State& s = st();
    if (s.prep && s.out) return true;
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const auto make = [](const uint32_t* code, size_t size, const VkDescriptorType* types, uint32_t n, uint32_t push,
                         VkPipelineLayout& layout, VkPipeline& pipe) {
        VkDescriptorSetLayoutBinding b[8]{};
        for (uint32_t i = 0; i < n; i++) b[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = n, li.pBindings = b;
        VkDescriptorSetLayout set{};
        if (vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set) != VK_SUCCESS) return false;
        const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout) != VK_SUCCESS) return false;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size, mi.pCode = code;
        VkShaderModule m{};
        if (vkCreateShaderModule(dev.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = layout;
        const bool ok = vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS;
        vkDestroyShaderModule(dev.device, m, nullptr);
        return ok;
    };
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType prep_types[5] = {S, S, I, I, I}, out_types[2] = {S, I};
    if (!make(DLSS_PREP_COMP_SPV, sizeof DLSS_PREP_COMP_SPV, prep_types, 5, 84, s.prep_layout, s.prep) ||
        !make(DLSS_OUT_COMP_SPV, sizeof DLSS_OUT_COMP_SPV, out_types, 2, 12, s.out_layout, s.out)) {
        s.prep = s.out = VK_NULL_HANDLE;
        return false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev.device, &si, nullptr, &s.point) != VK_SUCCESS) return false;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    return vkCreateSampler(dev.device, &si, nullptr, &s.linear) == VK_SUCCESS;
}

/// A view of a depth texture's depth alone, for sampling (made once per image)
VkImageView depth_view(Texture* t) {
    State& s = st();
    const uint64_t gen = texture_destroy_generation();
    if (gen != s.depth_views_gen) {   // (a texture was destroyed: its image's view may be stale)
        auto old = std::move(s.depth_views);
        s.depth_views.clear();
        later([old] {
            for (const auto& [image, view] : old) vkDestroyImageView(dev.device, view, nullptr);
        });
        s.depth_views_gen = gen;
    }
    if (auto it = s.depth_views.find(t->image); it != s.depth_views.end()) return it->second;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = t->image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = t->format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev.device, &vc, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    s.depth_views[t->image] = v;
    return v;
}

/// this frame's clip space to the frame before's: previous * inverse(current), row major; false when singular
bool reprojection(const float* current, const float* previous, float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? current[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {   // (Gauss-Jordan with partial pivoting)
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        if (p != c)
            for (int k = 0; k < 8; k++) std::swap(a[p][k], a[c][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c || a[r][c] == 0.0) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += previous[r * 4 + k] * a[k][c + 4];
            if (!std::isfinite(v)) return false;
            out[r * 4 + c] = (float)v;
        }
    return true;
}

/// DLSS's feature for this output size, made when the size changes (cmd: its creation's work)
bool make_feature(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    State& s = st();
    if (s.feature && s.feature_ow == ow && s.feature_oh == oh && rw <= s.feature_rw && rh <= s.feature_rh &&
        rw >= s.min_rw && rh >= s.min_rh)
        return true;
    if (s.feature) {
        NVSDK_NGX_Handle* old = s.feature;
        later([old] { NVSDK_NGX_VULKAN_ReleaseFeature(old); });
        s.feature = nullptr;
    }
    // (the quality mode whose range of render sizes holds this one: the network's 3/4 is DLSS's MaxQuality range)
    // (DLAA's when the render size is the output's)
    static const NVSDK_NGX_PerfQuality_Value scaled[] = {NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_DLAA,
                                                        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
                                                        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    static const NVSDK_NGX_PerfQuality_Value same[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality};
    const bool aa = rw == ow && rh == oh;
    const NVSDK_NGX_PerfQuality_Value* modes = aa ? same : scaled;
    const size_t nmodes = aa ? 2 : 5;
    NVSDK_NGX_PerfQuality_Value mode = modes[0];
    unsigned max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    bool found = aa;
    if (aa) max_w = min_w = ow, max_h = min_h = oh;   // (DLAA: the render size is the output's, one size)
    for (size_t i = 0; i < nmodes && !found; i++) {
        const NVSDK_NGX_PerfQuality_Value m = modes[i];
        unsigned w = 0, h = 0, xw = 0, xh = 0, nw = 0, nh = 0;
        float sharp = 0;
        if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(s.caps, ow, oh, m, &w, &h, &xw, &xh, &nw, &nh, &sharp)) || !w) continue;
        if (rw >= nw && rh >= nh && rw <= xw && rh <= xh) {
            mode = m, max_w = xw, max_h = xh, min_w = nw, min_h = nh, found = true;
            break;
        }
    }
    if (!found) return fail("no DLSS mode takes this render size");
    // (preset K, NVIDIA's transformer model, for every mode: RX_VK_DLSS_PRESET=<letter> another)
    unsigned preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    if (const char* p = getenv("RX_VK_DLSS_PRESET"); p && *p >= 'A' && *p <= 'Z') preset = (unsigned)(*p - 'A' + 1);
    for (const char* k : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        s.params->Set(k, preset);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = max_w, cp.Feature.InHeight = max_h, cp.Feature.InTargetWidth = ow, cp.Feature.InTargetHeight = oh;
    cp.Feature.InPerfQualityValue = mode;
    // (LDR colour, already tonemapped: no exposure; motion at the render size, not jittered; depth not reversed)
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cmd, 1, 1, &s.feature, s.params, &cp);
    if (NVSDK_NGX_FAILED(r) || !s.feature) {
        logf("vk: NVIDIA DLSS: its feature was not made (%#x) for %ux%u -> %ux%u", (unsigned)r, max_w, max_h, ow, oh);
        s.feature = nullptr;
        s.failed = true;
        return false;
    }
    s.feature_rw = max_w, s.feature_rh = max_h, s.feature_ow = ow, s.feature_oh = oh, s.min_rw = min_w, s.min_rh = min_h;
    logf("vk: NVIDIA DLSS: %ux%u (up to %ux%u) -> %ux%u, mode %d, preset %c", rw, rh, max_w, max_h, ow, oh, (int)mode,
         (char)('A' + preset - 1));
    s.last_frame = 0;   // (a new history)
    return true;
}

}  // namespace

bool dlss_built() { return true; }

void dlss_instance_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    if (env_switch("RX_VK_DLSS") == 0) return;
    for (const auto& e : all)
        if (!strcmp(e.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            want.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            return;
        }
}

void dlss_device_extensions(std::vector<const char*>& want, const std::vector<VkExtensionProperties>& all) {
    // (NGX's: VK_NVX_binary_import and VK_NVX_image_view_handle, with push descriptors and buffer device addresses,
    // which the replay enables already: core 1.2's bufferDeviceAddress stands for VK_EXT_buffer_device_address. The NVX ones
    // are on whenever the device has them, so the Insert menu's row needs no restart)
    if (env_switch("RX_VK_DLSS") == 0) return;
    bool both = true;
    for (const char* name : {VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME}) {
        bool have = false, listed = false;
        for (const auto& e : all) have |= !strcmp(e.extensionName, name);
        for (const char* w : want) listed |= !strcmp(w, name);
        if (have && !listed) want.push_back(name);
        both &= have;
    }
    // (NVIDIA's own extensions: without them this is not an NVIDIA GPU, or not one NGX runs on. With them, NGX says, in
    // dlss_probe or at the first upscale: a GTX has them too)
    if (!both) g_why = DLSS_NEEDS_RTX;
}

void dlss_probe() {
    if (dlss_why() != DLSS_OK || g_why.load(std::memory_order_relaxed) >= 0 || !dev.device) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    ngx_init();
}

void dlss_note_scene_draw(const Texture* color0, uint32_t width, uint32_t height, Texture* depth, uint64_t camera,
                          uint64_t camera_size) {
    if (!color0 || !depth || !camera || camera_size < 0x230 || width < 640 || height < 360) return;
    if (color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !dlss_wanted()) return;
    State& s = st();
    std::lock_guard lk(s.mu);
    const uint64_t now = texture_frame_no();
    if (s.cam.valid && s.cam.frame == now && (uint64_t)width * height <= (uint64_t)s.cam.width * s.cam.height)
        return;   // (the largest one of the frame wins)
    float c[0x230 / 4];
    if (!memory_read_as_submitted(camera, c, sizeof c)) return;
    // (the target's size at 0x120; the projection at 0x70, its w row (0, 0, -1, 0) at 0xa0)
    if (c[0x120 / 4] != (float)width || c[0x124 / 4] != (float)height || c[0xa0 / 4] != 0.0f || c[0xa4 / 4] != 0.0f ||
        c[0xa8 / 4] != -1.0f || c[0xac / 4] != 0.0f)
        return;
    static int told;
    if (told++ % 3000 == 0) {   // (now and then: what the camera's buffer held, to check its layout)
        std::string line;
        for (int i = 0; i < 0x230 / 4; i++) {
            char b[24];
            snprintf(b, sizeof b, "%s%g", i % 4 ? " " : i ? " | " : "", c[i]);
            line += b;
        }
        logf("vk: NVIDIA DLSS: the scene camera (%ux%u): %s", width, height, line.c_str());
    }
    Camera& k = s.cam;
    std::memcpy(k.current, c + 0x30 / 4, sizeof k.current);
    // (the offset sead's projection gave the frustum, in its third column: 2 * offset, which moves the picture by -that)
    k.jitter[0] = -c[0x78 / 4], k.jitter[1] = -c[0x88 / 4];
    k.width = width, k.height = height, k.depth = depth, k.depth_gen = texture_destroy_generation(), k.frame = now;
    k.valid = true;
}

uint32_t dlss_jitter_size(int axis) {
    // (RX_VK_DLSS_JITTER=0: none. 0 too once DLSS has not drawn for a while: the camera as the game has it)
    static const bool off = env_switch("RX_VK_DLSS_JITTER") == 0;
    State& s = st();
    if (off || !dlss_wanted() || s.failed || !s.last_frame || texture_frame_no() - s.last_frame > 30) return 0;
    return axis ? s.jitter_h.load(std::memory_order_relaxed) : s.jitter_w.load(std::memory_order_relaxed);
}

bool dlss_upscale(VkCommandBuffer cmd, uint64_t key, bool known, Texture* in, VkImageView in_view, Texture* out,
                  VkImageView out_view, uint32_t out_level) {
    if (!dlss_wanted()) return false;
    State& s = st();
    std::lock_guard lk(s.mu);
    if (!known) {   // (another network of the upscale's shape: only the scene into the picture, the rest untouched and unsaid)
        const Camera& c = s.cam;
        if (!in || !out || !c.valid || texture_frame_no() - c.frame > 1 || in->width != c.width || in->height != c.height ||
            out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || (out->width >> out_level) <= in->width)
            return false;
        static std::atomic<uint64_t> told{0};
        if (told.exchange(key) != key) logf("vk: NVIDIA DLSS: network %016llx is the gameplay upscale (%ux%u -> %ux%u)",
                                            (unsigned long long)key, in->width, in->height, out->width >> out_level,
                                            out->height >> out_level);
    }
    free_dead(false);
    const Camera cam = s.cam;
    s.cam.valid = false;   // (each frame's own camera)
    if (s.failed) return fail("DLSS failed earlier");
    if (!in || !in_view || !out || !out_view) return fail("no input or output view");
    if (!cam.valid || texture_frame_no() - cam.frame > 1) return fail("no scene camera this frame");
    if (cam.depth_gen != texture_destroy_generation()) return fail("the scene depth may be gone");
    const uint32_t rw = in->width, rh = in->height;
    const uint32_t ow = std::max(1u, out->width >> out_level), oh = std::max(1u, out->height >> out_level);
    if (rw != cam.width || rh != cam.height) return fail("the input is not the scene's size");
    if (cam.depth->width != rw || cam.depth->height != rh || !(cam.depth->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        cam.depth->samples != VK_SAMPLE_COUNT_1_BIT || !(cam.depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return fail("unexpected scene depth");
    if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || ow < rw || oh < rh || ow > rw * 3 || oh > rh * 3)
        return fail("unexpected output");
    if (!ngx_init()) {
        s.failed = true;
        return fail("DLSS not available");
    }
    if (!make_pipelines()) {
        s.failed = true;
        return fail("its shaders were not made");
    }
    const VkImageView dview = depth_view(cam.depth);
    if (!dview) return fail("no view of the scene depth");
    // (DLAA: DLSS at the render size, anti-aliasing alone; its picture then scaled to the output's by dlss_out.comp. With
    // it the 3D is drawn at full scale, dlss_full_scale)
    const bool dlaa = dlss_mode() == 2;
    const uint32_t dw = dlaa ? rw : ow, dh = dlaa ? rh : oh;
    if (!make_feature(cmd, rw, rh, dw, dh)) return false;
    if (!make_image(cmd, s.color, VK_FORMAT_R16G16B16A16_SFLOAT, s.feature_rw, s.feature_rh) ||
        !make_image(cmd, s.depth, VK_FORMAT_R32_SFLOAT, s.feature_rw, s.feature_rh) ||
        !make_image(cmd, s.motion, VK_FORMAT_R16G16_SFLOAT, s.feature_rw, s.feature_rh) ||
        !make_image(cmd, s.output, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dh))
        return fail("its images were not made");

    const uint64_t now = texture_frame_no();
    const bool reset = !s.last_frame || now - s.last_frame > 2;
    struct {
        float reproject[16];
        float jitter[2];
        int32_t size[2];
        int32_t flags;
    } pc{};
    static const bool show_motion = getenv("RX_VK_DLSS_DEBUG") && !strcmp(getenv("RX_VK_DLSS_DEBUG"), "motion");
    // (the motion without the jitter: from this frame's jittered clip space to the frame before's without its jitter.
    // A jitter moves clip x by jitter * w: taken off the previous matrix's first two rows)
    // (the frame before's camera as DLSS last drew it: the buffer's own slot for it, 0x180, holds zeros in this version)
    float previous[16];
    std::memcpy(previous, s.last_vp, sizeof previous);
    for (int c = 0; c < 4; c++) {
        previous[0 * 4 + c] -= s.last_jitter[0] * previous[3 * 4 + c];
        previous[1 * 4 + c] -= s.last_jitter[1] * previous[3 * 4 + c];
    }
    const bool motion = !reset && reprojection(cam.current, previous, pc.reproject);
    pc.jitter[0] = cam.jitter[0], pc.jitter[1] = cam.jitter[1];
    pc.size[0] = (int32_t)rw, pc.size[1] = (int32_t)rh, pc.flags = (motion ? 1 : 0) | (show_motion ? 2 : 0);

    full_barrier(cmd);   // (the scene's colour and depth, written before)
    {
        const VkDescriptorImageInfo info[5] = {{s.point, in_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {s.point, dview, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.color.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, s.motion.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[5]{};
        for (uint32_t i = 0; i < 5; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstBinding = i, w[i].descriptorCount = 1, w[i].pImageInfo = &info[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.prep_layout, 0, 5, w);
        vkCmdPushConstants(cmd, s.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    }
    full_barrier(cmd);
    NVSDK_NGX_Resource_VK color = resource(s.color, false), depth = resource(s.depth, false), mv = resource(s.motion, false),
                          output = resource(s.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color, ep.Feature.pInOutput = &output;
    ep.pInDepth = &depth, ep.pInMotionVectors = &mv;
    // (the jitter in render pixels, x right and y down; RX_VK_DLSS_JITTER_SIGN=x, y or xy: those turned round, to check)
    static const char* sign = getenv("RX_VK_DLSS_JITTER_SIGN") ? getenv("RX_VK_DLSS_JITTER_SIGN") : "";
    ep.InJitterOffsetX = cam.jitter[0] * 0.5f * (float)rw * (strchr(sign, 'x') ? -1.0f : 1.0f);
    ep.InJitterOffsetY = -cam.jitter[1] * 0.5f * (float)rh * (strchr(sign, 'y') ? -1.0f : 1.0f);
    ep.InRenderSubrectDimensions = {rw, rh};
    ep.InReset = reset ? 1 : 0;
    ep.InMVScaleX = ep.InMVScaleY = 1.0f;
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        logf("vk: NVIDIA DLSS: its evaluation failed (%#x): the game's upscale from now on", (unsigned)r);
        s.failed = true;
        descriptors_disturbed();
        return false;   // (the network writes its output over whatever the evaluation did)
    }
    full_barrier(cmd);
    {
        static const bool square = env_switch("RX_VK_DLSS_SQUARE") != 0;
        const int32_t opc[3] = {(int32_t)ow, (int32_t)oh, square && !show_motion ? 1 : 0};
        const VkDescriptorImageInfo info[2] = {{dlaa ? s.linear : s.point, s.output.view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, out_view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &info[0];
        w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &info[1];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out);
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.out_layout, 0, 2, w);
        vkCmdPushConstants(cmd, s.out_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof opc, opc);
        vkCmdDispatch(cmd, (ow + 7) / 8, (oh + 7) / 8, 1);
    }
    descriptors_disturbed();   // (the program's own pushes are gone from the bind point)
    s.last_frame = now;
    s.last_jitter[0] = cam.jitter[0], s.last_jitter[1] = cam.jitter[1];
    static int evaluations;
    if (++evaluations % 600 == 0)   // (now and then: the jitter DLSS was given, in render pixels)
        logf("vk: NVIDIA DLSS: %d frames drawn; this one's jitter %+.3f %+.3f px%s", evaluations, ep.InJitterOffsetX,
             ep.InJitterOffsetY, motion ? "" : ", no motion");
    std::memcpy(s.last_vp, cam.current, sizeof s.last_vp);
    s.jitter_w = rw, s.jitter_h = rh;
    if (s.reason) logf("vk: NVIDIA DLSS draws the upscale again (frame %llu)", (unsigned long long)now);
    s.reason = nullptr;
    if (!s.told_first) {
        s.told_first = true;
        logf("vk: NVIDIA DLSS draws OoT's upscale: %ux%u -> %ux%u%s", rw, rh, ow, oh, show_motion ? " (its motion shown)" : "");
    }
    return true;
}

#else  // (no NGX SDK in this build: DLSS is not there)

bool dlss_built() { return false; }
void dlss_probe() {}
void dlss_instance_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void dlss_device_extensions(std::vector<const char*>&, const std::vector<VkExtensionProperties>&) {}
void dlss_note_scene_draw(const Texture*, uint32_t, uint32_t, Texture*, uint64_t, uint64_t) {}
bool dlss_upscale(VkCommandBuffer, uint64_t, bool, Texture*, VkImageView, Texture*, VkImageView, uint32_t) { return false; }
uint32_t dlss_jitter_size(int) { return 0; }

#endif

} // namespace mkwvk
