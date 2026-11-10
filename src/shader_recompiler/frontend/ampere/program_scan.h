// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shader_recompiler/frontend/ampere/native_program_image.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace Shader::Ampere {

// A native program found in a shader container from the game's files (BNSH, agl BAHS, effect and
// layout archives): the same instruction range the runtime capture reads once the program is
// bound, so its NativeProgramKey and lowering match the bound program's.
struct ScannedProgram {
    size_t block_offset; ///< 256-byte block: the program header (graphics) or the code (compute)
    size_t code_offset;
    size_t code_size;
    bool graphics;
};

namespace ProgramScanDetail {
constexpr size_t BlockSize = 0x100;
constexpr size_t GraphicsCodeOffset = 0x80;
constexpr size_t MaximumBytes = ProgramCaptureState::MaximumInstructions * sizeof(Instruction);
// Every program ends with an unconditional branch to itself (BRA with a -16 offset).
constexpr u64 SelfBranchLow = 0xfffffff000007947ULL;

inline Instruction ReadInstruction(std::span<const u8> data, size_t offset) {
    Instruction instruction;
    std::memcpy(&instruction, data.data() + offset, sizeof(instruction));
    return instruction;
}

inline bool IsSelfBranch(std::span<const u8> data, size_t offset) {
    u64 low;
    std::memcpy(&low, data.data() + offset, sizeof(low));
    return low == SelfBranchLow;
}

inline bool IsZero(const Instruction& instruction) {
    return (instruction.words[0] | instruction.words[1] | instruction.words[2] |
            instruction.words[3]) == 0;
}

inline bool IsPadding(const Instruction& instruction) {
    return IsZero(instruction) || instruction.Bits(0, 12) == 0x918; // NOP
}

inline u32 ReadU32(std::span<const u8> data, size_t offset) {
    u32 value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

inline u64 ReadU64(std::span<const u8> data, size_t offset) {
    u64 value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

// Program header type in the low five bits of its first word (1 vertex/geometry, 2 pixel), and
// the zero tail between the 0x50-byte header and the code.
inline bool IsGraphicsHeader(std::span<const u8> data, size_t offset) {
    const u32 type = ReadU32(data, offset) & 0x1f;
    if (type != 1 && type != 2) {
        return false;
    }
    for (size_t tail = 0x50; tail < GraphicsCodeOffset; tail += sizeof(Instruction)) {
        if (!IsZero(ReadInstruction(data, offset + tail))) {
            return false;
        }
    }
    return true;
}

// Runs the runtime capture from code_offset; the program must end at the self-branch.
inline std::optional<size_t> CaptureSize(std::span<const u8> data, size_t code_offset,
                                         size_t branch_offset) {
    if (code_offset >= branch_offset || branch_offset - code_offset > MaximumBytes) {
        return std::nullopt;
    }
    ProgramCaptureState capture;
    const size_t limit = (branch_offset - code_offset) / sizeof(Instruction) + 1;
    for (size_t index = 0; index < limit; ++index) {
        const auto instruction = ReadInstruction(data, code_offset + index * sizeof(Instruction));
        switch (capture.Observe(instruction, static_cast<u32>(index))) {
        case ProgramCaptureState::Result::Continue:
            break;
        case ProgramCaptureState::Result::InvalidTarget:
            return std::nullopt;
        case ProgramCaptureState::Result::Complete: {
            const size_t end = code_offset + (index + 1) * sizeof(Instruction);
            if (end == branch_offset || end == branch_offset + sizeof(Instruction)) {
                return end - code_offset;
            }
            return std::nullopt;
        }
        }
    }
    return std::nullopt;
}

// Offsets of the 4-byte aligned container magics in data, in order.
inline std::vector<size_t> FindMagic(std::span<const u8> data, const char* magic) {
    std::vector<size_t> offsets;
    for (size_t offset = 0; offset + 4 <= data.size(); offset += sizeof(u32)) {
        if (std::memcmp(data.data() + offset, magic, 4) == 0) {
            offsets.push_back(offset);
        }
    }
    return offsets;
}
} // namespace ProgramScanDetail

// Finds native programs by their closing self-branch, then walks back to the first block after
// the previous program (or after a zero run, for the first one) and confirms the start by
// capturing forward, as the shader cache does, up to that self-branch.
inline std::vector<ScannedProgram> ScanNativePrograms(std::span<const u8> data) {
    using namespace ProgramScanDetail;
    std::vector<ScannedProgram> programs;
    size_t previous_end = 0;
    bool has_previous = false;
    for (size_t branch = 0; branch + sizeof(Instruction) <= data.size();
         branch += sizeof(Instruction)) {
        if (!IsSelfBranch(data, branch)) {
            continue;
        }
        size_t lower = branch > MaximumBytes ? branch - MaximumBytes : 0;
        if (has_previous && previous_end > lower) {
            lower = previous_end;
        } else {
            // First program in reach: walk back over its code to the zero run before it.
            size_t offset = branch;
            while (offset >= lower + sizeof(Instruction) &&
                   !IsZero(ReadInstruction(data, offset - sizeof(Instruction)))) {
                offset -= sizeof(Instruction);
            }
            lower = offset;
        }
        size_t first = lower;
        while (first < branch && IsPadding(ReadInstruction(data, first))) {
            first += sizeof(Instruction);
        }
        has_previous = true;
        previous_end = branch + sizeof(Instruction);

        // Capturing from too early a block also reaches the self-branch. Code holds no all-zero
        // instruction while headers and data between programs do, so the code starts after the
        // last zero slot; a block holding a graphics header wins over compute code.
        size_t code_floor = first;
        for (size_t offset = branch; offset > first; offset -= sizeof(Instruction)) {
            if (IsZero(ReadInstruction(data, offset - sizeof(Instruction)))) {
                code_floor = offset;
                break;
            }
        }
        const auto add = [&](size_t block, size_t size, bool graphics) {
            programs.push_back(ScannedProgram{
                .block_offset = block,
                .code_offset = block + (graphics ? GraphicsCodeOffset : 0),
                .code_size = size,
                .graphics = graphics,
            });
        };
        const size_t down = first / BlockSize * BlockSize;
        bool found = false;
        for (size_t block = down; block + GraphicsCodeOffset < branch; block += BlockSize) {
            if (block + GraphicsCodeOffset < code_floor || !IsGraphicsHeader(data, block)) {
                continue;
            }
            if (const auto size = CaptureSize(data, block + GraphicsCodeOffset, branch)) {
                add(block, *size, true);
                found = true;
                break;
            }
        }
        const size_t up = (code_floor + BlockSize - 1) / BlockSize * BlockSize;
        for (size_t block = up; !found && block < branch; block += BlockSize) {
            if (IsPadding(ReadInstruction(data, block))) {
                continue;
            }
            if (const auto size = CaptureSize(data, block, branch)) {
                add(block, *size, false);
                found = true;
            }
        }
    }
    return programs;
}

// The vertex and pixel programs combined by the shader program tables of BNSH containers: a
// stage entry holds the code block's offset from the container header and a size word tagged
// 0xd8 in its upper half; the pixel entry follows the vertex one 0x40 bytes later. A container's
// entries are looked for up to the next container.
inline std::vector<std::pair<size_t, size_t>> ScanProgramPairs(
    std::span<const u8> data, std::span<const ScannedProgram> programs) {
    using namespace ProgramScanDetail;
    constexpr size_t PixelEntryOffset = 0x40;
    constexpr u64 SizeTag = 0xd8;
    std::unordered_map<size_t, size_t> blocks; ///< Graphics block offset -> program index
    for (size_t index = 0; index < programs.size(); ++index) {
        if (programs[index].graphics) {
            blocks.emplace(programs[index].block_offset, index);
        }
    }
    const auto stage_type = [&](size_t index) { return data[programs[index].block_offset] & 0x1f; };
    std::vector<std::pair<size_t, size_t>> pairs;
    if (blocks.empty()) {
        return pairs;
    }
    const std::vector<size_t> bases = FindMagic(data, "BNSH");
    for (size_t container = 0; container < bases.size(); ++container) {
        const size_t base = bases[container];
        const size_t end = container + 1 < bases.size() ? bases[container + 1] : data.size();
        for (size_t entry = base; entry + PixelEntryOffset + sizeof(u64) <= end;
             entry += sizeof(u64)) {
            const auto vertex = blocks.find(base + ReadU64(data, entry));
            if (vertex == blocks.end() || (ReadU64(data, entry + sizeof(u64)) >> 32) != SizeTag) {
                continue;
            }
            const auto pixel = blocks.find(base + ReadU64(data, entry + PixelEntryOffset));
            if (pixel != blocks.end() && stage_type(vertex->second) == 1 &&
                stage_type(pixel->second) == 2) {
                pairs.emplace_back(vertex->second, pixel->second);
            }
        }
    }
    return pairs;
}

struct ScannedDispatch {
    size_t program;
    std::array<u32, 3> workgroup;
    u32 shared_memory;
    u32 local_memory;
};

// The workgroup, shared and local memory of compute programs, from the NVN shader control
// sections their stage entries lead to. An entry holds the code block's offset from its container
// (a BNSH header, or the code section of an agl BAHS archive) and a 0xd8 tag; the control section
// (magic 0x98761234) follows within 0x100 bytes, with the workgroup at +0x58, the shared memory
// at +0x64 and the local memory at +0x68. Programs whose code several entries share may get more
// than one dispatch. A container's entries are looked for up to the next container.
inline std::vector<ScannedDispatch> ScanComputeDispatches(
    std::span<const u8> data, std::span<const ScannedProgram> programs) {
    using namespace ProgramScanDetail;
    constexpr u32 ControlMagic = 0x98761234;
    constexpr size_t ControlReach = 0x100;
    constexpr size_t ControlSize = 0x70;
    constexpr size_t WorkgroupOffset = 0x58;
    constexpr size_t SharedMemoryOffset = 0x64;
    constexpr size_t LocalMemoryOffset = 0x68;
    constexpr u32 SharedMemoryGranularity = 0x100;
    std::unordered_map<size_t, size_t> blocks; ///< Compute code offset -> program index
    for (size_t index = 0; index < programs.size(); ++index) {
        if (!programs[index].graphics) {
            blocks.emplace(programs[index].code_offset, index);
        }
    }
    std::vector<ScannedDispatch> dispatches;
    if (blocks.empty()) {
        return dispatches;
    }
    // (where the container's entries start, what their code offsets count from): a BAHS archive
    // keeps its entries ahead of its code section.
    std::vector<std::pair<size_t, size_t>> containers;
    for (size_t offset = 0; offset + 0x20 <= data.size(); offset += sizeof(u32)) {
        if (std::memcmp(data.data() + offset, "BNSH", 4) == 0) {
            containers.emplace_back(offset, offset);
        } else if (std::memcmp(data.data() + offset, "BAHS", 4) == 0) {
            containers.emplace_back(offset, offset + ReadU32(data, offset + 0x1c));
        }
    }
    for (size_t container = 0; container < containers.size(); ++container) {
        const auto [start, base] = containers[container];
        const size_t end =
            container + 1 < containers.size() ? containers[container + 1].first : data.size();
        const size_t first = (start + sizeof(u64) - 1) / sizeof(u64) * sizeof(u64);
        for (size_t entry = first;
             entry < end && entry + ControlReach + ControlSize <= data.size();
             entry += sizeof(u64)) {
            const auto block = blocks.find(base + ReadU64(data, entry));
            if (block == blocks.end() ||
                (ReadU32(data, entry + 8) != 0xd8 && (ReadU64(data, entry + 8) >> 32) != 0xd8)) {
                continue;
            }
            for (size_t control = entry; control < entry + ControlReach; control += sizeof(u32)) {
                if (ReadU32(data, control) != ControlMagic) {
                    continue;
                }
                const std::array<u32, 3> workgroup{ReadU32(data, control + WorkgroupOffset),
                                                   ReadU32(data, control + WorkgroupOffset + 4),
                                                   ReadU32(data, control + WorkgroupOffset + 8)};
                const u64 invocations = u64{workgroup[0]} * workgroup[1] * workgroup[2];
                if (workgroup[0] == 0 || workgroup[1] == 0 || workgroup[2] == 0 ||
                    invocations > 1024) {
                    break;
                }
                const u64 shared = ReadU32(data, control + SharedMemoryOffset);
                dispatches.push_back(ScannedDispatch{
                    .program = block->second,
                    .workgroup = workgroup,
                    .shared_memory = static_cast<u32>((shared + SharedMemoryGranularity - 1) /
                                                      SharedMemoryGranularity *
                                                      SharedMemoryGranularity),
                    .local_memory = ReadU32(data, control + LocalMemoryOffset),
                });
                break;
            }
        }
    }
    return dispatches;
}

inline NativeProgramImage CopyNativeProgram(std::span<const u8> data,
                                            const ScannedProgram& program) {
    constexpr size_t HeaderSize = 0x50;
    NativeProgramImage image;
    if (program.graphics) {
        image.header.assign(data.begin() + program.block_offset,
                            data.begin() + program.block_offset + HeaderSize);
    }
    image.code.assign(data.begin() + program.code_offset,
                      data.begin() + program.code_offset + program.code_size);
    return image;
}

} // namespace Shader::Ampere
