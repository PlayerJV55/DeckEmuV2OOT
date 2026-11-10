// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
using Shader::Ampere::Instruction;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// ISBEWR.O [Ra + offset], Rdata; region 3 is .ATTR, 0 the map region.
Instruction Isbewr(u32 ra, u32 offset, u32 data, bool is_32, u32 region) {
    return {{0x7927U | (ra << 24), data | (offset << 8),
             (u32{is_32} << 11) | (region << 12) | (1U << 15), 0}};
}

// ISBERD.ATTR Rd, [Ra + offset] from the input region.
Instruction Isberd(u32 dest, u32 ra, u32 offset) {
    return {{0x7923U | (dest << 16) | (ra << 24), offset << 8, (1U << 11) | (3U << 12), 0}};
}

class MeshEnvironment final : public Shader::Environment {
public:
    MeshEnvironment(Shader::Stage stage_, std::span<const u64> code_, std::array<u32, 3> registers)
        : code{code_} {
        stage = stage_;
        mesh_registers = registers;
        // Output map: position x, y, z, w (system values b bits 4-7).
        sph.vtg.omap_systemb.raw = 0xf0;
    }
    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (address == 0x50 + index * 8 + ((index + 1) / 3) * 8) {
                return code[index];
            }
        }
        FAIL("Read outside lowered program at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        FAIL("Unexpected constant buffer read");
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {32, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    bool HasNativeInstructions() const override { return true; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};

std::vector<u64> Lower(std::span<const Instruction> program) {
    const auto lowered = Shader::Ampere::LowerToMaxwell(program);
    INFO(lowered.error);
    REQUIRE(lowered);
    return lowered.instructions;
}

std::vector<u32> Emit(Shader::Stage stage, std::span<const u64> code, std::array<u32, 3> registers) {
    MeshEnvironment env{stage, code, registers};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg,
                                                     Shader::HostTranslateInfo{});
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010400;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
}

// SPIR-V opcodes from the specification (SPV_EXT_mesh_shader).
constexpr u32 OpExecutionMode = 16;
constexpr u32 OpSetMeshOutputsEXT = 5295;

// Counts the instructions with this opcode in a SPIR-V module; with `mode`, only the
// OpExecutionMode instructions that set it.
size_t CountOp(std::span<const u32> code, u32 op, u32 mode = ~0U) {
    size_t count{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count{code[offset] >> 16};
        if (word_count == 0) {
            break;
        }
        count += (code[offset] & 0xffff) == op && (mode == ~0U || code[offset + 2] == mode);
        offset += word_count;
    }
    return count;
}

// SET_MESH_SHADER_A: triangles, 3 vertices, 1 primitive. SET_MESH_SHADER_B: 32 mesh threads.
// SET_MESH_INIT_SHADER: 32 task threads, one 128-byte line handed to the mesh stage.
constexpr std::array<u32, 3> MeshRegisters{4U | (3U << 4) | (1U << 13), 32U << 20,
                                           32U | (1U << 22)};
} // namespace

TEST_CASE("Mesh programs output points and lines as well as triangles", "[spirv][mesh]") {
    // The workgroup index and a payload word, one position line, the primitive count and
    // the first vertex index. Ocarina of Time's meshes output points (SET_MESH_SHADER_A 0x00040200).
    const auto code = Lower(std::array{Isberd(4, 255, 0x0), Isberd(5, 255, 0x10),
                                       Isbewr(255, 0x0, 5, true, 3),
                                       Isbewr(255, 0x3, 4, true, 0),
                                       Isbewr(255, 0x4, 255, false, 0),
                                       Isbewr(255, 0x5, 4, false, 0), Exit});
    // Execution modes OutputPoints 27, OutputLinesEXT 5269 and OutputTrianglesEXT 5298.
    for (const auto& [topology, mode] :
         {std::pair{0U, 27U}, std::pair{1U, 5269U}, std::pair{4U, 5298U}}) {
        auto registers = MeshRegisters;
        registers[0] = (registers[0] & ~0xfU) | topology;
        const auto spirv = Emit(Shader::Stage::Mesh, code, registers);
        CHECK(CountOp(spirv, OpExecutionMode, mode) == 1);
        CHECK(CountOp(spirv, OpSetMeshOutputsEXT) == 1);
    }
    // Line loops and strips are not mesh output topologies.
    auto registers = MeshRegisters;
    registers[0] = (registers[0] & ~0xfU) | 2U;
    CHECK_THROWS_AS(Emit(Shader::Stage::Mesh, code, registers), Shader::NotImplementedException);
}
