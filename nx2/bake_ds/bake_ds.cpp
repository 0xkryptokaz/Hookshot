// (bake-deepseek) Offline HKSEED01 -> SPIR-V -> hookshot_programs.bin, with no GPU, no game and no window.
//
// This is the CPU half of the bake: every stage the census enumerated is parsed exactly as the back end
// parses it at run time (parse_code, gpu/vk/vk_shader.cpp:155), lowered with the same Ampere -> Maxwell
// lowering (lower_to_maxwell.h), translated by the same Eden recompiler (TranslateProgram), and emitted by
// the same SPIR-V back end (EmitSPIRV). The environment answers are the ones the seed path already proves
// offline (seed_program, vk_shader.cpp:1893: a blank NvnState, attributes only for the bone-generic variants,
// no colour output types), so for compute and graphics this mirrors that path, not a new one.
//
// Outputs, under <outdir>:
//   spv/<program-key>.<nvn-stage>.spv   per stage
//   hookshot_programs.bin               the back end's program cache format (kDiskMagic 0x31505348 + stamp +
//                                       records exactly as program_cache_save writes them)
//   bake_report.jsonl                   one line per program attempt: key, kind, stages, descriptors, status
//   bake_manifest.json                  stamp inputs, counts, what was not translated and why
//
//   bake_ds translate --seed <hookshot_shaders.bin> --out <dir> [--stamp <hex16>] [--depth 0|1]
//                     [--variants 0|1|2|all] [--limit N] [--no-bank] [--program-size N]
//   bake_ds verify --bank <hookshot_programs.bin>
//
// The bank's standing: the header stamp is what a build of the SAME compiler/library computes for its
// Shader::Info/Program layout (disk_stamp, vk_shader.cpp:837). This tool computes it from its own layout, so a
// bank made by a libstdc++ build loads in a libstdc++ build and is refused (safely, whole-file) by a libc++ or
// MSVC one. --stamp overrides the translator half if a build pins another. verify re-reads a bank through a
// copy of program_cache_load's reader, so the format round-trips here; a load in the game itself is not tested.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

#include <vulkan/vulkan_core.h>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/oot_visual_workaround.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

// (live: vk_shader.cpp:57-69; the definitions live in the recompiler's passes)
namespace Shader::Optimization {
struct PointerDiag {
    uint32_t tracked, fallback, fallback_table, mixed, split, fallback_split;
};
extern thread_local PointerDiag g_pointer_diag;
extern thread_local uint32_t g_nvn2_compute_ssbo_base;
extern thread_local uint32_t g_nvn2_compute_ssbo_count;
extern thread_local uint32_t g_nvn2_compute_layout_end;
extern thread_local bool g_nvn2_miss_log_this;
extern thread_local bool g_all_global_this;
} // namespace Shader::Optimization

namespace {

constexpr uint32_t kControlMagic = 0x98761234;   // (live: vk_shader.cpp:95)
constexpr uint32_t kControlSize = 0xd8;
constexpr uint32_t kAmpereHeader = 128;
constexpr uint32_t kProgramHeaderWords = sizeof(Shader::ProgramHeader) / 8;   // 0x50 bytes
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = kFnvOffset) {   // (live: vk_shader.cpp:99)
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * kFnvPrime;
    return h;
}

// ---- NVN vertex attribute types (live: vk_shader.cpp:701 attribute_type_of; nvn_va_st fmt is the low byte) ----
Shader::AttributeType attribute_type_of(uint32_t vattr) {
    const uint32_t fmt = vattr & 0xFF;
    switch (fmt) {
    case 0x03: case 0x0F: case 0x1B: case 0x27: case 0x08: case 0x14: case 0x20: case 0x2C:
    case 0x0B: case 0x17: case 0x23: case 0x2F: case 0x3E:
        return Shader::AttributeType::UnsignedInt;
    case 0x04: case 0x10: case 0x1C: case 0x28: case 0x09: case 0x15: case 0x21: case 0x2D:
    case 0x0C: case 0x18: case 0x24: case 0x30: case 0x6A:
        return Shader::AttributeType::SignedInt;
    case 0x6B: return Shader::AttributeType::UnsignedScaled;
    case 0x6C: return Shader::AttributeType::SignedScaled;
    default:
        if (fmt >= 0x51 && fmt <= 0x68) return (fmt & 1) ? Shader::AttributeType::SignedScaled
                                                         : Shader::AttributeType::UnsignedScaled;
        return Shader::AttributeType::Float;
    }
}

struct StagePackage {
    int nvn_stage = -1;               // 0 vertex, 1 fragment, 5 compute, 6 task, 7 mesh
    uint64_t key = 0;
    std::vector<uint64_t> code;       // header (0x50 bytes, zeros for compute), Maxwell words, native address map
    std::vector<uint8_t> c1;
    uint32_t local[3]{};
    uint32_t shared_memory = 0;
    uint32_t texture_bound = ~0u;
    bool isbe = false;
    uint32_t mesh_threads = 0, payload_bytes = 0, max_vertices = 0, max_primitives = 0, topology = 0;
    bool header_only = false;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    bool ok = false;
    std::string error;                // empty when ok
};

bool control_end(const uint8_t* ctl, uint32_t* end_out) {   // (live: vk_shader.cpp:124)
    uint32_t magic, len, c1o, end;
    uint16_t st;
    std::memcpy(&magic, ctl, 4);
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&end, ctl + 0x44, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    if (magic != kControlMagic || !len || end > (16u << 20) || c1o > end || len > end || st > 7) return false;
    return *end_out = end, true;
}

/// The stage's control section and code area, parsed as parse_code's does (live: vk_shader.cpp:155).
/// Left out: the RX_VK_REJECT_* bisect lists, the RX_VK_NATIVE_MMA_KEYS choice (dev.native_mma is false here, so
/// HMMA lowers to the shuffle expansion), the native dumps and the logf lines.
StagePackage parse_stage(const uint8_t* ctl, const std::vector<uint8_t>& area) {
    StagePackage p;
    uint32_t len, c1o;
    const uint32_t end = static_cast<uint32_t>(area.size());
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4);
    std::memcpy(&c1o, ctl + 0x40, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    p.key = hash_bytes(area.data(), end, hash_bytes(ctl, kControlSize));
    p.nvn_stage = st;
    if (st == 6 || st == 7) {   // (the task/mesh control words, live: 160-168)
        uint32_t w58, w60;
        std::memcpy(&w58, ctl + 0x58, 4);
        std::memcpy(&w60, ctl + 0x60, 4);
        p.isbe = true;
        p.mesh_threads = w58 & 0xFFFF, p.payload_bytes = w58 >> 16;
        p.max_primitives = w60 & 0xFFFF, p.max_vertices = w60 >> 16;
        p.topology = ctl[0x5f];
    }
    const bool graphics = st != 5;
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    if (graphics) std::memcpy(p.sph, area.data(), std::min<size_t>(sizeof p.sph, area.size()));
    if (graphics && len <= hdr + 16) {   // (a header alone: the prim map stage of a mesh program)
        p.header_only = true;
        p.error = "header-only stage (mesh primitive map)";
        return p;
    }
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; hdr + (i + 1) * sizeof(Instruction) <= len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area.data() + hdr + i * sizeof(Instruction), sizeof insn);
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            p.error = "captured program's branch target is outside it";
            return p;
        }
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    Shader::Ampere::native_matrix_multiply = false;   // (live: dev.native_mma && !graphics && native_key)
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    if (!lowered) {
        char off[16];
        std::snprintf(off, sizeof off, "%#x", lowered.error_offset);
        p.error = std::string(lowered.error) + " (at +" + off + ")";
        return p;
    }
    p.texture_bound = lowered.required_texture_buffer;
    p.code.assign(kProgramHeaderWords, 0);
    if (graphics) std::memcpy(p.code.data(), area.data(), sizeof(Shader::ProgramHeader));
    for (const uint64_t instruction : lowered.instructions) {
        if (p.code.size() * 8 % 32 == 0) p.code.push_back(0);   // (the Maxwell scheduling word, live: 251)
        p.code.push_back(instruction);
    }
    p.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(p.code, lowered.source_instruction_offsets);
    p.c1.assign(area.begin() + c1o, area.end());
    if (!graphics) {
        std::memcpy(p.local, ctl + 0x58, 12);
        std::memcpy(&p.shared_memory, ctl + 0x64, 4);
    }
    p.ok = true;
    return p;
}

/// NVN2 compute's c0 layout words (live: vk_bank.cpp:49 CsLayout, read as seed_cs_control does, vk_bank.cpp:64)
struct CsLayout {
    uint32_t ubo = 0, ssbo = 0, tex = 0, img = 0, push = 0;
    uint32_t ubo_at() const { return 0x10 + push; }
    uint32_t ssbo_at() const { return ubo_at() + 16 * ubo; }
    uint32_t tex_at() const { return ssbo_at() + 16 * ssbo; }
    uint32_t img_at() const { return tex_at() + 8 * tex; }
    uint32_t end() const { return img_at() + 8 * img; }
};
CsLayout cs_layout_of(const uint8_t* c) {
    CsLayout l;
    l.ubo = c[0xb1 - 0x58], l.ssbo = c[0xb2 - 0x58], l.tex = c[0xb3 - 0x58], l.img = c[0xb6 - 0x58];
    l.push = (static_cast<uint32_t>(c[0xa9 - 0x58]) * 4 + 15) & ~15u;
    return l;
}

/// The translator's answers offline (live: vk_shader.cpp:357 Env). A live session answers from the draw's
/// state: driver banks c0/c2 (driver_bank_word), the uniform buffers' guest memory, registered textures. Here
/// nothing is bound, which is exactly what seed_program translates with (blank NvnState): c0/c2 read zero, c1
/// from the package, c3..c18 zero, texture slots answer Color2D and not-integer (an unbound handle's answer).
class OfflineEnv final : public Shader::Environment {
public:
    OfflineEnv(const StagePackage& p, Shader::Stage st) : nvn{p} {
        stage = st;
        start_address = st == Shader::Stage::Compute ? static_cast<u32>(kProgramHeaderWords * 8) : 0;
        if (st != Shader::Stage::Compute) {
            std::memcpy(&sph, nvn.code.data(), sizeof sph);
            local_memory = static_cast<uint32_t>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        }
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override {
        return address / 8 < nvn.code.size() ? nvn.code[address / 8] : 0;
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(nvn.code, address)) return *t;
        throw Shader::RuntimeError("indirect branch to native address {:#x}, not in the program", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {
            uint32_t v = 0;
            if (static_cast<size_t>(offset) + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
            return v;
        }
        return 0;   // c0/c2 not built (nothing bound), c3.. user uniform buffers unbound
    }
    Shader::TextureType ReadTextureType(u32) override {
        texture_questions++;
        return Shader::TextureType::Color2D;   // (an unbound slot's answer, live: vk_shader.cpp:394)
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat{}; }
    bool IsTexturePixelFormatInteger(u32) override {
        texture_questions++;
        return false;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override {
        if (nvn.texture_bound != ~0u) return nvn.texture_bound;
        return stage == Shader::Stage::Compute ? 0u : 2u;
    }
    u32 LocalMemorySize() const override { return local_memory; }
    u32 SharedMemorySize() const override {
        // (live: vk_shader.cpp:416; RX_VK_SHARED_48K=1 is the pre-PR-91 48 KB)
        static const bool old = std::getenv("RX_VK_SHARED_48K") != nullptr;
        return old ? 48u << 10 : nvn.shared_memory;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {std::max(1u, nvn.local[0]), std::max(1u, nvn.local[1]), std::max(1u, nvn.local[2])};
    }
    bool HasHLEMacroState() const override {
        // (live: vk_shader.cpp:430; RX_VK_NO_DRAW_PARAMS=1 is the pre-fix bank)
        static const bool off = std::getenv("RX_VK_NO_DRAW_PARAMS") != nullptr;
        return !off && stage == Shader::Stage::VertexB;
    }
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        // (live: 441: the word is a guest pointer or not. Every bank word but c1's reads zero; c1 holds the
        // program's own constants, and a mapping check is impossible offline, so false (as a blank state gives)
        const uint64_t key = static_cast<uint64_t>(index) << 32 | offset;
        if (const auto it = pointer_answers.find(key); it != pointer_answers.end()) return it->second;
        pointer_answers.emplace(key, false);
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        if (bank != 0 || !HasHLEMacroState()) return std::nullopt;
        switch (offset) {
        case 0x640: return Shader::ReplaceConstant::BaseVertex;
        case 0x644: return Shader::ReplaceConstant::BaseInstance;
        case 0x648: return Shader::ReplaceConstant::DrawID;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}
    /// (live: vk_shader.cpp:459 Env::set_mesh)
    void set_mesh(const std::array<u32, 3>& registers, const uint8_t* primitive_sph) {
        mesh_registers = registers;
        if (primitive_sph) std::memcpy(&mesh_primitive_header, primitive_sph, sizeof mesh_primitive_header);
    }

    int texture_questions = 0;

private:
    const StagePackage& nvn;
    uint32_t local_memory = 0;
    std::unordered_map<uint64_t, bool> pointer_answers;
};

uint64_t fnv_loop(const uint8_t* b, size_t n, uint64_t h = kFnvOffset) { return hash_bytes(b, n, h); }

/// The device limit the SPIR-V's storage-buffer address masking is baked with (live: dev.props.limits.
/// minStorageBufferOffsetAlignment). Offline it is a guess; --ssbo-align sets it. 16 (the Vulkan minimum) is
/// the safe lower bound: the mask only clears bits the descriptor address is known to have clear, so a
/// smaller value can never round a real base down (MiMo's offline translator made the same choice; GLM's
/// draft and my first cut used 64, which can move a 16/32-aligned descriptor base). A bank translated with
/// another device's limit is still the same program key, but its masked bases differ.
uint64_t g_ssbo_align = 16;

Shader::Profile make_profile() {   // (live: vk_shader.cpp:476; device properties faked)
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = 0x7FFFFFFFu;   // VK_SHADER_STAGE_ALL
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = false;
    p.support_scaled_attributes = false;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = false;
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = g_ssbo_align;   // (dev.props.limits.minStorageBufferOffsetAlignment)
    return p;
}

Shader::HostTranslateInfo host_info() {   // (live: vk_shader.cpp:508)
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    h.support_float16 = std::getenv("RX_VK_NO_F16") == nullptr;   // (live: vk_shader.cpp:510)
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = g_ssbo_align;
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;
    return h;
}

/// (live: vk_shader.cpp:805 oot_visual; off unless RX_VK_OOT_VISUAL is set, which is also what the cache stamp sees)
void oot_visual(Shader::IR::Program& program, uint64_t key) {
    static const bool on = std::getenv("RX_VK_OOT_VISUAL") && std::atoi(std::getenv("RX_VK_OOT_VISUAL")) != 0;
    if (!on) return;
    static const float fog = std::getenv("RX_VK_OOT_FOG") ? static_cast<float>(std::atof(std::getenv("RX_VK_OOT_FOG"))) : 0.08f;
    static const float exposure =
        std::getenv("RX_VK_OOT_EXPOSURE") ? static_cast<float>(std::atof(std::getenv("RX_VK_OOT_EXPOSURE"))) : 1.0f;
    Shader::Optimization::ApplyOotVisualWorkaround(program, fog, exposure);
    (void)key;
}

struct Translation {
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
    Shader::IR::Program program;
};

std::unique_ptr<Translation> translate(OfflineEnv& env, bool graphics, bool one_exit = false) {   // (live: vk_shader.cpp:532)
    auto t = std::make_unique<Translation>();
    Shader::Maxwell::Flow::CFG cfg{env, t->flow_blocks,
                                   static_cast<u32>(env.StartAddress() + (graphics ? sizeof(Shader::ProgramHeader) : 0u)),
                                   one_exit};
    t->program = Shader::Maxwell::TranslateProgram(t->insts, t->blocks, env, cfg, host_info());
    return t;
}

uint32_t read_word(const uint8_t* b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b + at, 4);
    return v;
}

/// HKSEED01 as seed_open reads it (live: vk_shader.cpp:1844)
struct SeedFile {
    std::vector<uint8_t> blob;
    struct Entry {
        uint32_t stage = 0;
        uint64_t key = 0;
        size_t control = 0, code = 0;
        uint32_t bytes = 0, end = 0;
        uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    };
    std::vector<Entry> stages;
    std::vector<std::vector<uint32_t>> programs;
    std::string error;
};

SeedFile read_seed(const char* path) {
    SeedFile seed;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        seed.error = "cannot open " + std::string(path);
        return seed;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    seed.blob.resize(static_cast<size_t>(size));
    if (size < 16 || std::fread(seed.blob.data(), 1, seed.blob.size(), f) != seed.blob.size() ||
        std::memcmp(seed.blob.data(), "HKSEED01", 8)) {
        seed.error = "not a HKSEED01 file";
        std::fclose(f);
        return seed;
    }
    std::fclose(f);
    const uint32_t nprograms = read_word(seed.blob.data(), 8), nstages = read_word(seed.blob.data(), 12);
    size_t at = 16;
    seed.stages.resize(nstages);
    for (auto& st : seed.stages) {
        if (at + 20 > seed.blob.size()) {
            seed.error = "stage table runs past the file";
            return seed;
        }
        const uint8_t* h = seed.blob.data() + at;
        const uint32_t csize = read_word(h, 12), bytes = read_word(h, 16);
        st.stage = read_word(h, 8);
        st.control = at + 20;
        st.code = st.control + csize;
        at = st.code + bytes;
        if (at > seed.blob.size()) {
            seed.error = "stage bytes run past the file";
            return seed;
        }
        if (csize >= kControlSize && control_end(seed.blob.data() + st.control, &st.end) && st.end <= bytes) {
            st.key = hash_bytes(seed.blob.data() + st.code, st.end,
                                hash_bytes(seed.blob.data() + st.control, kControlSize));
            std::memcpy(st.sph, seed.blob.data() + st.code, std::min<size_t>(sizeof st.sph, st.end));
        } else {
            st.end = 0;   // (not a control section this back end reads)
        }
    }
    seed.programs.resize(nprograms);
    for (auto& list : seed.programs) {
        if (at + 4 > seed.blob.size()) {
            seed.error = "program table runs past the file";
            return seed;
        }
        const uint32_t n = read_word(seed.blob.data(), at);
        at += 4;
        if (at + static_cast<size_t>(n) * 4 > seed.blob.size()) {
            seed.error = "program list runs past the file";
            return seed;
        }
        list.resize(n);
        for (uint32_t k = 0; k < n; k++) list[k] = read_word(seed.blob.data(), at + k * 4);
        at += static_cast<size_t>(n) * 4;
    }
    return seed;
}

// ---- keys (live: vk_shader.cpp:733 graphics_key, :744 mesh_key) ----
const Shader::ProgramHeader& header_of(const StagePackage& c) {
    return *reinterpret_cast<const Shader::ProgramHeader*>(c.sph);
}
uint64_t graphics_key(const StagePackage& vs, const StagePackage* fs, const uint32_t vattr[16],
                      const uint8_t color_types[8]) {
    uint64_t key = vs.key ^ (fs ? fs->key * 31 : 0);
    for (int i = 0; i < 16; i++) {
        const auto used = header_of(vs).vtg.InputGeneric(static_cast<size_t>(i));
        const bool read = used[0] || used[1] || used[2] || used[3];
        key = key * kFnvPrime ^ (static_cast<uint64_t>(read ? attribute_type_of(vattr[i])
                                                            : Shader::AttributeType::Float) << i);
    }
    uint8_t written[8]{};
    for (int i = 0; fs && i < 8; i++) written[i] = header_of(*fs).ps.HasOutputComponents(i) ? color_types[i] : 0;
    return hash_bytes(written, 8, key);
}
uint64_t mesh_key(const StagePackage* task, const StagePackage& mesh, const StagePackage* fs, const uint8_t color_types[8]) {
    uint64_t key = hash_bytes("mesh", 4, mesh.key);
    if (task) key = key * kFnvPrime ^ task->key;
    if (fs) key = key * kFnvPrime ^ fs->key;
    uint8_t written[8]{};
    for (int i = 0; fs && i < 8; i++) written[i] = header_of(*fs).ps.HasOutputComponents(i) ? color_types[i] : 0;
    return hash_bytes(written, 8, key);
}

// ---- the program cache's record format (live: vk_shader.cpp:1051-1113, program_cache_save/get_info) ----
struct Answer {
    uint16_t stage, desc;
    uint8_t type;
    bool image;
};
// (a layout mirror of mkwvk::Stage/Program for disk_stamp's sizeof(Program) word: the same members, types and
// order as vk_shader.h, with Vulkan's own handle types. A build whose struct differs is refused by the stamp
// (all-or-nothing), never misread)
struct StageLayout {
    int nvn_stage = 0;
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;
    Shader::Info info;
    std::vector<uint8_t> c1;
};
struct ProgramLayout {
    uint64_t key = 0;
    bool compute = false, failed = false;
    std::atomic<bool> ready{false};
    std::vector<StageLayout> stages;
    std::array<uint32_t, 3> workgroup{1, 1, 1};
    uint32_t shared_memory = 0;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorType> binding_types;
    VkPipeline compute_pipeline = VK_NULL_HANDLE;
    std::atomic<int> compute_state{0};
    bool push = false;
    VkShaderStageFlags push_stages = 0;
    bool uses_render_area = false;
    bool uses_push = true;
    bool mesh = false;
    std::vector<Answer> answers;
    uint64_t variant = 0;
    mutable std::atomic<int64_t> wm_sig{-1};
};

uint64_t disk_stamp(uint64_t translator_stamp, uint32_t depth) {   // (live: vk_shader.cpp:837)
    uint64_t h = kFnvOffset;
    const auto add = [&h](const void* d, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(d);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * kFnvPrime;
    };
    add(&translator_stamp, sizeof translator_stamp);
    add(&depth, sizeof depth);
    const uint32_t abi[] = {static_cast<uint32_t>(sizeof(Shader::Info)),
                            static_cast<uint32_t>(sizeof(ProgramLayout)),
                            static_cast<uint32_t>(sizeof(std::vector<uint32_t>)),
                            3u, 0u};   // libstdc++ (a libc++ or MSVC build writes 1/2 and its version)
    add(abi, sizeof abi);
    for (char** e = environ; e && *e; e++)
        for (const char* pre : {"RX_VK_OOT", "RX_VK_MMA", "RX_VK_NATIVE", "RX_VK_NAN", "RX_VK_TASK_CAP", "RX_VK_NETWORK",
                                "RX_VK_GP", "RX_VK_FOLLOW", "RX_VK_NO_FOLLOW", "RX_VK_NO_F16", "RX_VK_SHARED", "RX_VK_MESH",
                                "EDEN_"})
            if (!std::strncmp(*e, pre, std::strlen(pre))) add(*e, std::strlen(*e));
    return h;
}

void put_bytes(std::vector<uint8_t>& o, const void* d, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(d);
    o.insert(o.end(), b, b + n);
}
template <class T> void put(std::vector<uint8_t>& o, const T& v) { put_bytes(o, &v, sizeof v); }
template <class V> void put_list(std::vector<uint8_t>& o, const V& v) {
    put(o, static_cast<uint64_t>(v.size()));
    for (const auto& e : v) put(o, e);
}
/// Shader::Info is plain data but for its heap members (boost vectors); they go after the raw bytes, as live does
void put_info(std::vector<uint8_t>& o, const Shader::Info& i) {
    put_bytes(o, &i, sizeof i);
    put_list(o, i.constant_buffer_descriptors);
    put_list(o, i.storage_buffers_descriptors);
    put_list(o, i.texture_buffer_descriptors);
    put_list(o, i.image_buffer_descriptors);
    put_list(o, i.texture_descriptors);
    put_list(o, i.image_descriptors);
    put(o, static_cast<uint64_t>(i.legacy_stores_mapping.size()));
    for (const auto& kv : i.legacy_stores_mapping) put(o, kv.first), put(o, kv.second);
    put(o, static_cast<uint64_t>(i.handle_programs.size()));
    for (const auto& h : i.handle_programs) put_list(o, h);
}

struct Reader {   // (live: vk_shader.cpp:1000; the same boundaries)
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;
    bool has(uint64_t n) {
        if (!bad && n > static_cast<uint64_t>(end - p)) bad = true;
        return !bad;
    }
    template <class T> T get() {
        alignas(T) uint8_t b[sizeof(T)]{};
        if (has(sizeof(T))) {
            std::memcpy(b, p, sizeof(T));
            p += sizeof(T);
        }
        return *reinterpret_cast<T*>(b);
    }
    uint64_t count(size_t elem) {
        const uint64_t n = get<uint64_t>();
        if (!bad && n > static_cast<uint64_t>(end - p) / (elem ? elem : 1)) bad = true;
        return bad ? 0 : n;
    }
    template <class V> void list(V& v) {
        const uint64_t n = count(sizeof(typename V::value_type));
        v.clear();
        v.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n && !bad; i++) v.push_back(get<typename V::value_type>());
    }
};
void get_info(Reader& r, Shader::Info& i) {
    if (!r.has(sizeof i)) return;
    alignas(Shader::Info) static thread_local uint8_t fresh[sizeof(Shader::Info)];
    std::memcpy(fresh, static_cast<const void*>(&i), sizeof i);
    std::memcpy(static_cast<void*>(&i), r.p, sizeof i);
    r.p += sizeof i;
#define KEEP(m) std::memcpy(static_cast<void*>(&i.m), fresh + (reinterpret_cast<const uint8_t*>(&i.m) - reinterpret_cast<const uint8_t*>(&i)), sizeof i.m)
    KEEP(constant_buffer_descriptors);
    KEEP(storage_buffers_descriptors);
    KEEP(texture_buffer_descriptors);
    KEEP(image_buffer_descriptors);
    KEEP(texture_descriptors);
    KEEP(image_descriptors);
    KEEP(legacy_stores_mapping);
    KEEP(handle_programs);
#undef KEEP
    r.list(i.constant_buffer_descriptors);
    r.list(i.storage_buffers_descriptors);
    r.list(i.texture_buffer_descriptors);
    r.list(i.image_buffer_descriptors);
    r.list(i.texture_descriptors);
    r.list(i.image_descriptors);
    for (uint64_t n = r.count(2 * sizeof(Shader::IR::Attribute)); n && !r.bad; n--) {
        const auto k = r.get<Shader::IR::Attribute>();
        i.legacy_stores_mapping[k] = r.get<Shader::IR::Attribute>();
    }
    i.handle_programs.resize(static_cast<size_t>(r.count(sizeof(uint64_t))));
    for (auto& h : i.handle_programs) r.list(h);
}

// ---- answers (live: vk_shader.cpp:604-668). Offline the banks are blank, so handles read 0 and the plain
//      answer is the descriptor's own type, exactly as an unbound slot is recorded live.
uint32_t stage_handle(const Shader::Info& info, uint32_t program, OfflineEnv& env) {
    if (program >= info.handle_programs.size()) return 0;
    return Shader::EvaluateHandleProgram(
               info.handle_programs[program],
               [&](u32 index, u32 offset) -> u32 { return env.ReadCbufValue(index, offset); },
               [&](u32 binding, u32) -> u32 {
                   if (binding >= info.storage_buffers_descriptors.size()) return 0;
                   const auto& d = info.storage_buffers_descriptors[binding];
                   const uint64_t base = env.ReadCbufValue(d.cbuf_index, d.cbuf_offset) |
                                         static_cast<uint64_t>(env.ReadCbufValue(d.cbuf_index, d.cbuf_offset + 4)) << 32;
                   return base ? 0u : 0u;   // (a storage buffer's guest words are unbound offline: zero)
               })
        .value_or(0u);
}
uint32_t descriptor_handle(const Shader::Info& info, const Shader::TextureDescriptor& d, OfflineEnv& env) {
    if (d.handle_program != ~0U) return stage_handle(info, d.handle_program, env);
    uint32_t h = env.ReadCbufValue(d.cbuf_index, d.cbuf_offset);
    if (d.has_secondary)
        h = h << d.shift_left | env.ReadCbufValue(d.secondary_cbuf_index, d.secondary_cbuf_offset) << d.secondary_shift_left;
    return h;
}
uint32_t descriptor_handle(const Shader::Info& info, const Shader::ImageDescriptor& d, OfflineEnv& env) {
    if (d.handle_program != ~0U) return stage_handle(info, d.handle_program, env);
    return env.ReadCbufValue(d.cbuf_index, d.cbuf_offset);
}
uint8_t texture_answer(uint32_t handle) {   // (live: 618; an unregistered or zero handle is Color2D, not integer)
    return handle ? static_cast<uint8_t>(Shader::TextureType::Color2D) : 0xFF;
}
void record_answers(std::vector<Answer>& answers, const std::vector<Shader::Info>& infos, OfflineEnv* envs[]) {
    answers.clear();
    for (size_t k = 0; k < infos.size(); k++) {
        const auto& list = infos[k].texture_descriptors;
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].count == 1) {
                const uint32_t h = descriptor_handle(infos[k], list[i], *envs[k]);
                answers.push_back({static_cast<uint16_t>(k), static_cast<uint16_t>(i),
                                   h ? texture_answer(h)
                                     : static_cast<uint8_t>(static_cast<uint8_t>(list[i].type) |
                                                            (list[i].is_integer ? 0x80 : 0))});
            }
        const auto& images = infos[k].image_descriptors;
        for (size_t i = 0; i < images.size(); i++)
            if (images[i].count == 1) {
                const uint32_t h = descriptor_handle(infos[k], images[i], *envs[k]);
                answers.push_back({static_cast<uint16_t>(k), static_cast<uint16_t>(i),
                                   h ? static_cast<uint8_t>(texture_answer(h) & 0x80)
                                     : static_cast<uint8_t>(images[i].is_integer ? 0x80 : 0),
                                   true});
            }
    }
}
uint64_t answers_hash(const std::vector<Answer>& answers) {   // (live: vk_shader.cpp:642)
    uint64_t h = 0;
    for (const Answer& a : answers)
        h = (h ^ (static_cast<uint64_t>(a.stage) << 32 | static_cast<uint64_t>(a.desc) << 16 |
                  static_cast<uint64_t>(a.type) << 1 | static_cast<uint64_t>(a.image))) * kFnvPrime;
    return h;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (const char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += c;
            }
        }
    }
    return o;
}
std::string hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

struct StageResult {
    int nvn_stage = -1;
    uint32_t vk_stage = 0;   // 0x10 vertex, 0x20 fragment, 0x20? compute 0x20? (set below)
    std::vector<uint32_t> spirv;
    std::vector<uint8_t> c1;
    Shader::Info info;
};
/// (live: vk_shader.cpp:1298 mesh_registers_of)
std::array<uint32_t, 3> mesh_registers_of(const StagePackage* task, const StagePackage& mesh) {
    const uint32_t topology = mesh.topology == 3 ? 4 : mesh.topology == 1 ? 0 : mesh.topology;
    const uint32_t lines = task ? (task->payload_bytes + 127) / 128 : 0;
    const uint32_t most = std::max(mesh.max_vertices, mesh.max_primitives);
    return {(topology & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13),
            std::min(mesh.mesh_threads, 0x3Fu) << 20,
            (task ? std::min(task->mesh_threads, 0x3Fu) : 0) | (std::min(lines, 0x1FFu) << 12) |
                (std::min(lines, 0x1FFu) << 22)};
}

struct ProgramResult {
    uint64_t key = 0;
    bool compute = false, mesh = false;
    std::vector<StageResult> stages;
    std::array<uint32_t, 3> workgroup{1, 1, 1};
    uint32_t shared_memory = 0;
    std::vector<Answer> answers;
    uint64_t variant = 0;
};

/// A seed stage, parsed once per stage index (the seed lists each once)
struct ParsedStage {
    StagePackage pkg;
    bool parsed = false;
};

constexpr uint32_t kVkVertex = 0x00000001;     // VK_SHADER_STAGE_VERTEX_BIT
constexpr uint32_t kVkFragment = 0x00000010;   // VK_SHADER_STAGE_FRAGMENT_BIT
constexpr uint32_t kVkCompute = 0x00000020;    // VK_SHADER_STAGE_COMPUTE_BIT
constexpr uint32_t kVkTask = 0x00000040;       // VK_SHADER_STAGE_TASK_BIT_EXT
constexpr uint32_t kVkMesh = 0x00000080;       // VK_SHADER_STAGE_MESH_BIT_EXT

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  bake_ds translate --seed <hookshot_shaders.bin> --out <dir> [--stamp <hex16>] [--depth 0|1]\n"
                 "                    [--variants 0|1|2|all] [--limit N] [--no-bank] [--program-size N] [--ssbo-align N]\n"
                 "  bake_ds verify --bank <hookshot_programs.bin>\n");
    return 2;
}

int verify_bank(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "verify: cannot open %s\n", path);
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    const long long whole = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    uint32_t head[2]{};
    uint64_t stamp = 0;
    if (whole < 16 || std::fread(head, 4, 2, f) != 2 || std::fread(&stamp, 8, 1, f) != 1 || head[0] != 0x31505348) {
        std::fprintf(stderr, "verify: not a program cache (bad header)\n");
        std::fclose(f);
        return 1;
    }
    std::vector<uint8_t> data(static_cast<size_t>(whole) - 16);
    std::fseek(f, 16, SEEK_SET);
    const bool whole_read = std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!whole_read) {
        std::fprintf(stderr, "verify: short read\n");
        return 1;
    }
    size_t at = 0;
    size_t records = 0, stages = 0, words = 0, bad = 0;
    while (at + 16 <= data.size()) {
        uint64_t key, size;
        std::memcpy(&key, data.data() + at, 8);
        std::memcpy(&size, data.data() + at + 8, 8);
        at += 16;
        if (at + size > data.size()) {
            std::fprintf(stderr, "verify: record %s runs past the file (%llu > %zu)\n", hex64(key).c_str(),
                         static_cast<unsigned long long>(size), data.size() - at);
            bad++;
            break;
        }
        Reader r{data.data() + at, data.data() + at + size};
        const uint32_t kind = r.get<uint32_t>();
        std::vector<Answer> answers;
        r.list(answers);
        const auto wg = r.get<std::array<uint32_t, 3>>();
        const uint32_t shared = r.get<uint32_t>();
        const uint64_t n = r.count(1);
        bool ok = !r.bad && n >= 1 && n <= 8;
        Shader::Info info{};
        for (uint64_t k = 0; ok && k < n; k++) {
            const int nvn_stage = r.get<int>();
            const uint32_t vk_stage = r.get<uint32_t>();
            std::vector<uint32_t> spirv;
            std::vector<uint8_t> c1;
            r.list(spirv);
            r.list(c1);
            get_info(r, info);
            words += spirv.size();
            stages++;
            ok = !r.bad && !spirv.empty();
            (void)nvn_stage;
            (void)vk_stage;
        }
        if (!ok || r.bad) {
            bad++;
            std::fprintf(stderr, "verify: record %s unreadable\n", hex64(key).c_str());
        } else {
            records++;
            (void)wg;
            (void)shared;
        }
        at += size;
    }
    std::printf("verify: magic 0x31505348, stamp %s, %zu records, %zu stages, %zu SPIR-V words, %zu bad, %zu trailing bytes\n",
                hex64(stamp).c_str(), records, stages, words, bad, data.size() - at);
    return bad ? 1 : 0;
}

// ---- the translation pass ----

struct Options {
    std::string seed, out;
    uint64_t stamp = ~0ull;
    uint32_t depth = 0;
    int variants = -1;   // (0: input variant 0 only; 1: 0 and 1; 2: 0,1,2; -1: all, prepare_run's programs*3 default)
    int limit = 0;
    bool bank = true;
    long program_size = 0;
    uint64_t ssbo_align = 16;
};

struct AttrVariant {
    uint32_t attributes[16]{};
    int index = 0;
};

std::vector<AttrVariant> attr_variants(const Options& o) {
    std::vector<AttrVariant> v;
    v.push_back({});
    const int most = o.variants < 0 ? 2 : o.variants;
    for (int i = 1; i <= most; i++) {
        AttrVariant a;
        a.index = i;
        a.attributes[6] = 0x27;   // RGBA8UI in nvn_va_st.fmt (NVN_FORMAT_RGBA8UI = 0x27)
        if (i >= 2) a.attributes[7] = 0x27;
        v.push_back(a);
    }
    return v;
}

bool reads_generic(const StagePackage& vs, int g) {
    const auto used = header_of(vs).vtg.InputGeneric(static_cast<size_t>(g));
    return used[0] || used[1] || used[2] || used[3];
}

void write_u64(FILE* f, uint64_t v) { std::fwrite(&v, 8, 1, f); }

int run_translate(const Options& o) {
    SeedFile seed = read_seed(o.seed.c_str());
    if (!seed.error.empty()) {
        std::fprintf(stderr, "bake_ds: %s\n", seed.error.c_str());
        return 2;
    }
    g_ssbo_align = o.ssbo_align ? o.ssbo_align : 16;
    std::error_code ec;
    std::filesystem::create_directories(o.out + "/spv", ec);
    // (HOOKSHOT_SHADER_STAMP is the translator half of the stamp; the tool's default 0 is for format tests.
    // --stamp takes the value nx2/build.py computed and the game logged.)
    const uint64_t disk = disk_stamp(o.stamp == ~0ull ? 0 : o.stamp, o.depth);
    std::fprintf(stderr, "bake_ds: %zu programs, %zu stages; translator stamp %s -> cache stamp %s; sizeof(Info) %zu, "
                         "sizeof(ProgramLayout) %zu\n",
                 seed.programs.size(), seed.stages.size(), hex64(o.stamp == ~0ull ? 0 : o.stamp).c_str(), hex64(disk).c_str(),
                 sizeof(Shader::Info), sizeof(ProgramLayout));
    FILE* bank = nullptr;
    std::string bank_path = o.out + "/hookshot_programs.bin";
    std::vector<uint8_t> bank_head;
    if (o.bank) {
        bank = std::fopen(bank_path.c_str(), "wb");
        if (!bank) {
            std::fprintf(stderr, "bake_ds: cannot write %s\n", bank_path.c_str());
            return 2;
        }
        const uint32_t h[2] = {0x31505348, 0};
        std::fwrite(h, 4, 2, bank);
        write_u64(bank, disk);
        std::fflush(bank);
    }
    FILE* report = std::fopen((o.out + "/bake_report.jsonl").c_str(), "w");
    if (!report) {
        std::fprintf(stderr, "bake_ds: cannot write the report\n");
        if (bank) std::fclose(bank);
        return 2;
    }
    std::vector<ParsedStage> parsed(seed.stages.size());
    const auto pkg_of = [&](uint32_t idx) -> const StagePackage& {
        ParsedStage& ps = parsed[idx];
        if (!ps.parsed) {
            const auto& e = seed.stages[idx];
            if (!e.end) {
                ps.pkg.error = "not a control section";
            } else {
                std::vector<uint8_t> area(seed.blob.begin() + e.code, seed.blob.begin() + e.code + e.end);
                ps.pkg = parse_stage(seed.blob.data() + e.control, area);
            }
            ps.parsed = true;
        }
        return ps.pkg;
    };

    size_t translated = 0, failed = 0, skipped = 0, program_slots = 0;
    std::vector<AttrVariant> variants = attr_variants(o);
    const uint8_t no_types[8]{};
    for (size_t pi = 0; pi < seed.programs.size(); pi++) {
        if (o.limit > 0 && static_cast<int>(pi) >= o.limit) break;
        // (the program's stages by NVN stage, as seed_program does; a stage that is not a control section drops the program)
        const StagePackage* by_stage[8]{};
        const char* bad_status = nullptr;
        std::string bad_what;
        uint32_t bad_stage = 0;
        for (const uint32_t k : seed.programs[pi]) {
            if (k >= seed.stages.size() || !seed.stages[k].end) {
                bad_status = "INVALID_STAGE";
                bad_what = "not a control section this back end reads";
                bad_stage = k < seed.stages.size() ? seed.stages[k].stage : 0;
                break;
            }
            const StagePackage& pkg = pkg_of(k);
            if (!pkg.ok) {
                bad_status = pkg.header_only ? "HEADER_ONLY" : "LOWER_ERROR";
                bad_what = pkg.error;
                bad_stage = seed.stages[k].stage;
                break;
            }
            by_stage[seed.stages[k].stage & 7] = &pkg;
        }
        if (bad_status) {   // (status names follow bake-mimo's stop-line taxonomy)
            skipped++;
            std::fprintf(report, "{\"program\":%zu,\"status\":\"%s\",\"stage\":%u,\"what\":\"%s\"}\n",
                         pi, bad_status, bad_stage, json_escape(bad_what).c_str());
            continue;
        }
        const StagePackage* one = by_stage[5] ? by_stage[5] : by_stage[7] ? by_stage[7] : by_stage[0];
        if (!one) {
            skipped++;
            std::fprintf(report, "{\"program\":%zu,\"status\":\"no-vertex-or-compute\"}\n", pi);
            continue;
        }
        const bool compute = by_stage[5] != nullptr;
        const bool mesh = !compute && by_stage[7] != nullptr;
        for (const AttrVariant& av : variants) {
            if (av.index && (compute || mesh)) continue;
            if (av.index && (!reads_generic(*by_stage[0], 6) || (av.index == 2 && !reads_generic(*by_stage[0], 7)))) continue;
            program_slots++;
            ProgramResult pr;
            pr.compute = compute;
            const uint64_t key = compute ? by_stage[5]->key
                                 : mesh ? mesh_key(by_stage[6], *by_stage[7], by_stage[1], no_types)
                                        : graphics_key(*by_stage[0], by_stage[1], av.attributes, no_types);
            pr.key = key;
            std::string err;
            int questions = 0;
            CsLayout cs{};   // (compute c0 layout, for the bank plan)
            if (mesh) {
                // (live: vk_shader.cpp:1320 translate_mesh_into with prim = null, as seed_program calls it)
                const std::array<uint32_t, 3> regs = mesh_registers_of(by_stage[6], *by_stage[7]);
                pr.mesh = true;
                std::unique_ptr<Translation> task_t;
                if (by_stage[6]) {
                    StageResult s6;
                    OfflineEnv env6{*by_stage[6], Shader::Stage::Task};
                    env6.set_mesh(regs, nullptr);
                    task_t = translate(env6, true, true);
                    questions += env6.texture_questions;
                    Shader::RuntimeInfo rt6{};
                    rt6.previous_stage_stores.mask.set();
                    s6.info = task_t->program.info;
                    s6.nvn_stage = 6;
                    s6.c1 = by_stage[6]->c1;
                    Shader::Backend::Bindings b6{};
                    s6.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), rt6, task_t->program, b6);
                    if (s6.spirv.empty()) err = "task EmitSPIRV returned no words";
                    s6.vk_stage = kVkTask;
                    pr.stages.push_back(std::move(s6));
                }
                StageResult sm;
                OfflineEnv menv{*by_stage[7], Shader::Stage::Mesh};
                menv.set_mesh(regs, nullptr);
                auto mesh_t = translate(menv, true, true);
                questions += menv.texture_questions;
                Shader::RuntimeInfo mrt{};
                if (task_t) mrt.previous_stage_stores = task_t->program.info.stores;
                else mrt.previous_stage_stores.mask.set();
                mrt.convert_depth_mode = o.depth != 0;
                sm.info = mesh_t->program.info;
                sm.nvn_stage = 7;
                sm.c1 = by_stage[7]->c1;
                Shader::Backend::Bindings bm{};
                sm.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), mrt, mesh_t->program, bm);
                if (sm.spirv.empty()) err = "mesh EmitSPIRV returned no words";
                sm.vk_stage = kVkMesh;
                pr.stages.push_back(std::move(sm));
                if (err.empty() && by_stage[1]) {
                    OfflineEnv fenv{*by_stage[1], Shader::Stage::Fragment};
                    auto fs = translate(fenv, true);
                    questions += fenv.texture_questions;
                    Shader::RuntimeInfo frt{};
                    frt.previous_stage_stores = mesh_t->program.info.stores;
                    for (const uint32_t line : mesh_t->program.mesh.primitive_lines) {
                        const auto attribute = static_cast<Shader::IR::Attribute>(line);
                        if (Shader::IR::IsGeneric(attribute)) frt.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
                    }
                    if (mesh_t->program.mesh.topology == 0)
                        for (size_t i = 0; i < Shader::IR::NUM_GENERICS; i++)
                            if (fs->program.info.loads.Generic(i) && !mesh_t->program.info.stores.Generic(i))
                                frt.point_coord_generics.set(i);
                    for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(no_types[i]);
                    oot_visual(fs->program, by_stage[1]->key);
                    uint32_t float_targets = 0;
                    for (int i = 0; i < 8; i++)
                        if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
                    static const bool nan_guard2 = !std::getenv("RX_VK_NAN_GUARD") || std::atoi(std::getenv("RX_VK_NAN_GUARD")) != 0;
                    if (nan_guard2) Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
                    StageResult f;
                    f.info = fs->program.info;
                    f.nvn_stage = 1;
                    f.c1 = by_stage[1]->c1;
                    Shader::Backend::Bindings fb{};
                    f.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), frt, fs->program, fb);
                    if (f.spirv.empty()) err = "fragment EmitSPIRV returned no words";
                    f.vk_stage = kVkFragment;
                    pr.stages.push_back(std::move(f));
                }
            }
            if (!compute && !mesh) {
                Shader::RuntimeInfo vrt{};
                OfflineEnv env{*by_stage[0], Shader::Stage::VertexB};
                auto t = translate(env, true);
                questions += env.texture_questions;
                if (!t->program.info.stores[Shader::IR::Attribute::PointSize]) vrt.fixed_state_point_size = 1.0f;
                for (int i = 0; i < 16; i++) vrt.generic_input_types[i] = attribute_type_of(av.attributes[i]);
                vrt.previous_stage_stores.mask.set();
                vrt.convert_depth_mode = o.depth != 0;
                StageResult v;
                v.info = t->program.info;
                v.nvn_stage = 0;
                v.c1 = by_stage[0]->c1;
                Shader::Backend::Bindings bindings{};
                v.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), vrt, t->program, bindings);
                if (v.spirv.empty()) err = "vertex EmitSPIRV returned no words";
                v.vk_stage = kVkVertex;
                pr.stages.push_back(std::move(v));
                if (err.empty() && by_stage[1]) {
                    OfflineEnv fenv{*by_stage[1], Shader::Stage::Fragment};
                    auto ft = translate(fenv, true);
                    questions += fenv.texture_questions;
                    oot_visual(ft->program, by_stage[1]->key);
                    Shader::RuntimeInfo frt{};
                    frt.previous_stage_stores = t->program.info.stores;
                    for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(no_types[i]);
                    uint32_t float_targets = 0;
                    for (int i = 0; i < 8; i++)
                        if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
                    static const bool nan_guard = !std::getenv("RX_VK_NAN_GUARD") || std::atoi(std::getenv("RX_VK_NAN_GUARD")) != 0;
                    if (nan_guard) Shader::Optimization::GuardFragmentOutputs(ft->program, float_targets);
                    StageResult f;
                    f.info = ft->program.info;
                    f.nvn_stage = 1;
                    f.c1 = by_stage[1]->c1;
                    Shader::Backend::Bindings fbindings{};
                    f.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), frt, ft->program, fbindings);
                    if (f.spirv.empty()) err = "fragment EmitSPIRV returned no words";
                    f.vk_stage = kVkFragment;
                    pr.stages.push_back(std::move(f));
                }
            }
            if (compute) {
                StageResult sr;
                // (the compute stage's own control section; it is stage 5 of the program)
                const uint8_t* ctl5 = nullptr;
                for (const uint32_t k : seed.programs[pi])
                    if ((seed.stages[k].stage & 7) == 5) ctl5 = seed.blob.data() + seed.stages[k].control;
                const CsLayout l5 = cs_layout_of(ctl5 + 0x58);
                cs = l5;
                OfflineEnv env{*by_stage[5], Shader::Stage::Compute};
                Shader::Optimization::g_nvn2_compute_ssbo_base = l5.ssbo ? l5.ssbo_at() : 0;
                Shader::Optimization::g_nvn2_compute_ssbo_count = l5.ssbo ? l5.ssbo : 16;
                Shader::Optimization::g_nvn2_compute_layout_end = l5.ssbo ? l5.end() : 0;
                Shader::Optimization::g_nvn2_miss_log_this = true;
                Shader::Optimization::g_all_global_this = false;
                struct Reset {
                    ~Reset() {
                        Shader::Optimization::g_nvn2_compute_ssbo_base = 0;
                        Shader::Optimization::g_nvn2_compute_ssbo_count = 16;
                        Shader::Optimization::g_nvn2_compute_layout_end = 0;
                        Shader::Optimization::g_all_global_this = false;
                    }
                } reset;
                try {
                    auto t = translate(env, false);
                    questions += env.texture_questions;
                    oot_visual(t->program, by_stage[5]->key);
                    Shader::RuntimeInfo rt{};
                    sr.info = t->program.info;
                    sr.nvn_stage = 5;
                    sr.c1 = by_stage[5]->c1;
                    Shader::Backend::Bindings bindings{};
                    sr.spirv = Shader::Backend::SPIRV::EmitSPIRV(make_profile(), rt, t->program, bindings);
                    if (sr.spirv.empty()) err = "compute EmitSPIRV returned no words";
                    pr.workgroup = {t->program.workgroup_size[0], t->program.workgroup_size[1], t->program.workgroup_size[2]};
                    pr.shared_memory = t->program.shared_memory_size;
                } catch (const std::exception& e) {
                    err = e.what();
                }
                sr.vk_stage = kVkCompute;
                pr.stages.push_back(std::move(sr));
            }
            if (!err.empty()) {
                failed++;
                std::fprintf(report,
                             "{\"program\":%zu,\"key\":\"%s\",\"kind\":\"%s\",\"variant\":%d,\"status\":\"translate-failed\","
                             "\"what\":\"%s\"}\n",
                             pi, hex64(key).c_str(), compute ? "compute" : mesh ? "mesh" : "graphics", av.index, json_escape(err).c_str());
                continue;
            }
            // answers, as record_answers would leave them for this translation
            std::vector<Shader::Info> infos;
            std::vector<std::unique_ptr<OfflineEnv>> envs_held;
            std::vector<OfflineEnv*> envs;
            for (size_t k = 0; k < pr.stages.size(); k++) {
                const StagePackage& pkg = pr.compute ? *by_stage[5]
                                          : mesh ? (k == 0 && by_stage[6] ? *by_stage[6]
                                                    : pr.stages[k].nvn_stage == 7 ? *by_stage[7] : *by_stage[1])
                                                 : (k == 0 ? *by_stage[0] : *by_stage[1]);
                envs_held.push_back(std::make_unique<OfflineEnv>(pkg, Shader::Stage::Compute));
                envs.push_back(envs_held.back().get());
                infos.push_back(pr.stages[k].info);
            }
            record_answers(pr.answers, infos, envs.data());
            pr.variant = answers_hash(pr.answers);
            // write SPIR-V
            for (auto& s : pr.stages) {
                const std::string path = o.out + "/spv/" + hex64(key) + "." + std::to_string(s.nvn_stage) + ".spv";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(s.spirv.data(), 4, s.spirv.size(), f);
                    std::fclose(f);
                }
            }
            // bank record (live: vk_shader.cpp:1047 program_cache_save)
            if (bank) {
                std::vector<uint8_t> o2;
                put(o2, static_cast<uint32_t>((pr.compute ? 1u : 0u) | (pr.mesh ? 2u : 0u)));
                put_list(o2, pr.answers);
                put(o2, pr.workgroup);
                put(o2, pr.shared_memory);
                put(o2, static_cast<uint64_t>(pr.stages.size()));
                for (auto& s : pr.stages) {
                    put(o2, static_cast<int32_t>(s.nvn_stage));
                    put(o2, s.vk_stage);
                    put_list(o2, s.spirv);
                    put_list(o2, s.c1);
                    put_info(o2, s.info);
                }
                const uint64_t head[2] = {key, o2.size()};
                std::fwrite(head, 8, 2, bank);
                std::fwrite(o2.data(), 1, o2.size(), bank);
                std::fflush(bank);
            }
            translated++;
            // report line with the descriptor plan (what a pipeline layout/bank must bind)
            std::fprintf(report,
                         "{\"program\":%zu,\"key\":\"%s\",\"kind\":\"%s\",\"variant\":%d,\"status\":\"translated\","
                         "\"workgroup\":[%u,%u,%u],\"shared_memory\":%u,\"answers\":%zu,\"questions\":%d,\"stages\":[",
                         pi, hex64(key).c_str(), compute ? "compute" : mesh ? "mesh" : "graphics", av.index, pr.workgroup[0], pr.workgroup[1],
                         pr.workgroup[2], pr.shared_memory, pr.answers.size(), questions);
            for (size_t k = 0; k < pr.stages.size(); k++) {
                const auto& s = pr.stages[k];
                std::fprintf(report,
                             "%s{\"nvn_stage\":%d,\"spirv_words\":%zu,\"cbufs\":[", k ? "," : "", s.nvn_stage, s.spirv.size());
                for (size_t d = 0; d < s.info.constant_buffer_descriptors.size(); d++) {
                    const auto& c = s.info.constant_buffer_descriptors[d];
                    std::fprintf(report, "%s[%u,%u]", d ? "," : "", c.index, c.count);
                }
                std::fprintf(report, "],\"ssbos\":[");
                for (size_t d = 0; d < s.info.storage_buffers_descriptors.size(); d++) {
                    const auto& b = s.info.storage_buffers_descriptors[d];
                    std::fprintf(report, "%s[%u,%u,%u,%d]", d ? "," : "", b.cbuf_index, b.cbuf_offset, b.count,
                                 b.is_written ? 1 : 0);
                }
                std::fprintf(report, "],\"textures\":[");
                for (size_t d = 0; d < s.info.texture_descriptors.size(); d++) {
                    const auto& t = s.info.texture_descriptors[d];
                    std::fprintf(report, "%s[%u,%u,%u,%u,%u]", d ? "," : "", t.cbuf_index, t.cbuf_offset,
                                 static_cast<uint32_t>(t.type), t.is_integer ? 1 : 0, t.count);
                }
                std::fprintf(report, "],\"images\":[");
                for (size_t d = 0; d < s.info.image_descriptors.size(); d++) {
                    const auto& im = s.info.image_descriptors[d];
                    std::fprintf(report, "%s[%u,%u,%u,%u]", d ? "," : "", im.cbuf_index, im.cbuf_offset,
                                 im.is_integer ? 1 : 0, im.count);
                }
                std::fprintf(report, "],\"c1_bytes\":%zu,\"uses_fp16\":%d,\"uses_fp64\":%d,\"uses_cm\":%d,\"local_memory\":%u}",
                             s.c1.size(), s.info.uses_fp16 ? 1 : 0, s.info.uses_fp64 ? 1 : 0,
                             s.info.uses_cooperative_matrix ? 1 : 0, s.info.uses_local_memory ? 1 : 0);
            }
            if (compute)
                std::fprintf(report,
                             "],\"cs\":{\"push_words\":%u,\"push_bytes\":%u,\"ubo\":%u,\"ssbo\":%u,\"tex\":%u,\"img\":%u,"
                             "\"ubo_at\":%u,\"ssbo_at\":%u,\"tex_at\":%u,\"img_at\":%u,\"end\":%u}}\n",
                             static_cast<uint32_t>(cs.push / 4), cs.push, cs.ubo, cs.ssbo, cs.tex, cs.img, cs.ubo_at(),
                             cs.ssbo_at(), cs.tex_at(), cs.img_at(), cs.end());
            else
                std::fprintf(report, "]}\n");
        }
    }
    if (bank) std::fclose(bank);
    std::fclose(report);
    FILE* mf = std::fopen((o.out + "/bake_manifest.json").c_str(), "w");
    if (mf) {
        std::fprintf(mf,
                     "{\n  \"tool\": \"bake_ds\",\n  \"seed\": \"%s\",\n  \"seed_stages\": %zu,\n  \"seed_programs\": %zu,\n"
                     "  \"program_slots\": %zu,\n  \"translated\": %zu,\n  \"failed\": %zu,\n  \"skipped\": %zu,\n"
                     "  \"cache_stamp\": \"%s\",\n  \"translator_stamp\": \"%s\",\n  \"depth_mode\": %u,\n"
                     "  \"sizeof_info\": %zu,\n  \"sizeof_program_layout\": %zu,\n  \"ssbo_alignment\": %llu,\n  \"bank\": %s\n}\n",
                     json_escape(o.seed).c_str(), seed.stages.size(), seed.programs.size(), program_slots, translated, failed,
                     skipped, hex64(disk).c_str(), hex64(o.stamp == ~0ull ? 0 : o.stamp).c_str(), o.depth,
                     sizeof(Shader::Info), sizeof(ProgramLayout), static_cast<unsigned long long>(g_ssbo_align),
                     o.bank ? "\"hookshot_programs.bin\"" : "null");
        std::fclose(mf);
    }
    std::fprintf(stderr, "bake_ds: %zu slots: %zu translated, %zu failed, %zu programs skipped (unreadable/mesh)\n", program_slots,
                 translated, failed, skipped);
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string_view cmd = argv[1];
    if (cmd == "verify") {
        const char* bank = nullptr;
        for (int i = 2; i < argc; i++)
            if (!std::strcmp(argv[i], "--bank") && i + 1 < argc) bank = argv[++i];
        if (!bank) return usage();
        return verify_bank(bank);
    }
    if (cmd != "translate") return usage();
    Options o;
    for (int i = 2; i < argc; i++) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seed") o.seed = next();
        else if (a == "--out") o.out = next();
        else if (a == "--stamp") o.stamp = std::strtoull(next(), nullptr, 16);
        else if (a == "--depth") o.depth = static_cast<uint32_t>(std::atoi(next()));
        else if (a == "--variants") {
            const std::string_view v = next();
            o.variants = v == "all" ? -1 : std::atoi(std::string(v).c_str());
        } else if (a == "--limit") o.limit = std::atoi(next());
        else if (a == "--no-bank") o.bank = false;
        else if (a == "--program-size") o.program_size = std::atol(next());
        else if (a == "--ssbo-align") o.ssbo_align = std::strtoull(next(), nullptr, 0);
        else return usage();
    }
    if (o.seed.empty() || o.out.empty()) return usage();
    return run_translate(o);
}
