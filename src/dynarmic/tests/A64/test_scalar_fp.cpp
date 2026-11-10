// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Scalar float and double operations: their operands come from lane 0 of a vector register (the
// other lanes hold garbage here), their results clear the rest of the register, and they reach
// general registers, memory and the flags. The x64 backend can keep such scalars in XMM registers
// (UserConfig::scalars_in_xmm).

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"

using namespace Dynarmic;
using namespace oaknut::util;

namespace {

class HaltingEnv final : public A64TestEnv {
public:
    A64::Jit* jit = nullptr;

    void ExceptionRaised(u64, A64::Exception) override {
        jit->HaltExecution();
    }
};

u64 F(float value) {
    return std::bit_cast<u32>(value);
}

u64 D(double value) {
    return std::bit_cast<u64>(value);
}

struct RoundResults {
    u64 frinta_s, frintx_s, frinta_d, frintx_d;
    u32 fpsr;
};

class RoundJit {
public:
    explicit RoundJit(bool inline_fp_round) {
        A64::UserConfig conf{};
        conf.callbacks = &env;
        conf.enable_cycle_counting = false;
        conf.inline_fp_round = inline_fp_round;
        jit.emplace(conf);
        env.jit = &*jit;

        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        code.FRINTA(S1, S0);
        code.FRINTX(S2, S0);
        code.FRINTA(D4, D3);
        code.FRINTX(D5, D3);
        code.BRK(0);
    }

    RoundResults Run(u32 fpcr, u32 single, u64 double_) {
        jit->SetPC(0);
        jit->SetFpcr(fpcr);
        jit->SetFpsr(0);
        jit->SetVector(0, {0x5555'5555'0000'0000ULL | single, 0x0123'4567'89AB'CDEF});
        jit->SetVector(3, {double_, 0xFEDC'BA98'7654'3210});
        CheckedRun([&]() { jit->Run(); });
        return {jit->GetVector(1)[0], jit->GetVector(2)[0], jit->GetVector(4)[0], jit->GetVector(5)[0], jit->GetFpsr()};
    }

private:
    HaltingEnv env;
    std::optional<A64::Jit> jit;
};

}  // namespace

TEST_CASE("A64: scalar float and double operations on lane 0", "[a64]") {
    for (const bool scalars_in_xmm : {false, true}) {
        for (const bool cycle_counting : {false, true}) {
            HaltingEnv env;
            A64::UserConfig conf{};
            conf.callbacks = &env;
            conf.enable_cycle_counting = cycle_counting;
            conf.scalars_in_xmm = scalars_in_xmm;
            A64::Jit jit{conf};
            env.jit = &jit;

            oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
            oaknut::Label less;
            code.FMUL(S0, S1, S2);       // -3.375
            code.FMADD(S3, S0, S1, S2);  // -3.375 * 1.5 - 2.25 = -7.3125
            code.FADD(S4, S3, S0);       // -10.6875
            code.FSUB(S5, S4, S1);       // -12.1875
            code.FMOV(W6, S5);
            code.STR(S4, X10);
            code.LDR(S7, X10);
            code.FMUL(S8, S7, S1);  // -16.03125
            code.FCMP(S8, S4);      // less
            code.FCSEL(S11, S1, S2, oaknut::Cond::LT);
            code.FMUL(D12, D9, D19);  // 1.5
            code.FADD(D13, D12, D9);  // 4.5
            code.FMOV(X14, D13);
            code.STR(D13, X10, 8);
            code.FCVT(D15, S4);
            code.FMOV(S16, S1);
            code.B(oaknut::Cond::LT, less);
            code.MOVZ(X17, 2);
            code.BRK(0);
            code.l(less);
            code.MOVZ(X17, 1);
            code.BRK(0);

            for (int run = 0; run < 2; ++run) {
                for (size_t i = 0; i < 32; ++i) {
                    jit.SetVector(i, {0x5555'5555'5555'5555 + i, 0xAAAA'AAAA'AAAA'AAAA - i});
                }
                jit.SetVector(1, {0xDEAD'BEEF'0000'0000 | F(1.5f), 0x0123'4567'89AB'CDEF});
                jit.SetVector(2, {0xCAFE'F00D'0000'0000 | F(-2.25f), 0xFEDC'BA98'7654'3210});
                jit.SetVector(9, {D(3.0), 0x1111'2222'3333'4444});
                jit.SetVector(19, {D(0.5), 0x5555'6666'7777'8888});
                jit.SetRegister(10, 0x1000);
                jit.SetPC(0);
                env.ticks_left = 100;
                CheckedRun([&]() { jit.Run(); });

                INFO("scalars in xmm " << scalars_in_xmm << " cycle counting " << cycle_counting << " run " << run);
                CHECK(jit.GetVector(0) == Vector{F(-3.375f), 0});
                CHECK(jit.GetVector(3) == Vector{F(-7.3125f), 0});
                CHECK(jit.GetVector(4) == Vector{F(-10.6875f), 0});
                CHECK(jit.GetVector(5) == Vector{F(-12.1875f), 0});
                CHECK(jit.GetRegister(6) == F(-12.1875f));
                CHECK(env.MemoryRead(0x1000, 4) == F(-10.6875f));
                CHECK(jit.GetVector(7) == Vector{F(-10.6875f), 0});
                CHECK(jit.GetVector(8) == Vector{F(-16.03125f), 0});
                CHECK(jit.GetVector(11) == Vector{F(1.5f), 0});
                CHECK(jit.GetVector(12) == Vector{D(1.5), 0});
                CHECK(jit.GetVector(13) == Vector{D(4.5), 0});
                CHECK(jit.GetRegister(14) == D(4.5));
                CHECK(env.MemoryRead(0x1008, 8) == D(4.5));
                CHECK(jit.GetVector(15) == Vector{D(-10.6875), 0});
                CHECK(jit.GetVector(16) == Vector{F(1.5f), 0});
                CHECK(jit.GetRegister(17) == 1);
                CHECK(jit.GetPstate() == 0x80000000);  // N: less
                // Registers the code does not write keep their lanes.
                CHECK(jit.GetVector(1) == Vector{0xDEAD'BEEF'0000'0000 | F(1.5f), 0x0123'4567'89AB'CDEF});
                CHECK(jit.GetVector(20) == Vector{0x5555'5555'5555'5555 + 20, 0xAAAA'AAAA'AAAA'AAAA - 20});
            }
        }
    }
}

TEST_CASE("A64: scalar FRINTA and FRINTX inline give the soft-float results and flags", "[a64]") {
    RoundJit soft{false};
    RoundJit inline_{true};

    constexpr std::array<u32, 30> singles{
        0x00000000, 0x80000000, 0x3E99999A /* 0.3 */, 0xBE99999A /* -0.3 */, 0x3F000000 /* 0.5 */,
        0xBF000000 /* -0.5 */, 0x3EFFFFFF /* 0.49999997 */, 0x3FC00000 /* 1.5 */, 0x40200000 /* 2.5 */,
        0xC0200000 /* -2.5 */, 0x40600000 /* 3.5 */, 0x4AFFFFFF /* 8388607.5 */, 0xCAFFFFFF /* -8388607.5 */,
        0x4AFFFFFD /* 8388606.5 */, 0x4B000000 /* 2^23 */, 0x4B7FFFFF /* 16777215 */, 0x7149F2CA /* 1e30 */,
        0xF149F2CA /* -1e30 */, 0x7F800000 /* inf */, 0xFF800000 /* -inf */, 0x7FC00001 /* qNaN */,
        0x7F800001 /* sNaN */, 0xFF800002 /* -sNaN */, 0x00000001 /* denormal */, 0x80400000 /* -denormal */,
        0x3F7FFFFF /* 0.99999994 */, 0x3F800001 /* 1.0000001 */, 0x4A800001 /* 4194304.5 */,
        0x3F400000 /* 0.75 */, 0xBF400000 /* -0.75 */,
    };
    constexpr std::array<u64, 24> doubles{
        0x0000000000000000, 0x8000000000000000, 0x3FE0000000000000 /* 0.5 */, 0xBFE0000000000000 /* -0.5 */,
        0x4004000000000000 /* 2.5 */, 0xC004000000000000 /* -2.5 */, 0x3FDFFFFFFFFFFFFF /* 0.49999999999999994 */,
        0x432FFFFFFFFFFFFF /* 2^52 - 0.5 */, 0xC32FFFFFFFFFFFFF /* -(2^52 - 0.5) */, 0x4330000000000000 /* 2^52 */,
        0x432FFFFFFFFFFFFD /* 2^52 - 1.5 */, 0x7E37E43C8800759C /* 1e300 */, 0x7FF0000000000000 /* inf */,
        0xFFF0000000000000 /* -inf */, 0x7FF8000000000001 /* qNaN */, 0x7FF0000000000001 /* sNaN */,
        0x0000000000000001 /* denormal */, 0x800FFFFFFFFFFFFF /* -denormal */, 0x3FD3333333333333 /* 0.3 */,
        0xBFD3333333333333 /* -0.3 */, 0x3FF8000000000000 /* 1.5 */, 0x400C000000000000 /* 3.5 */,
        0x3FE8000000000000 /* 0.75 */, 0x4130000000000001 /* 1048576.0000000002 */,
    };
    // Round to nearest, towards +inf, towards -inf, towards zero, and default NaN.
    for (const u32 fpcr : {0x00000000u, 0x00400000u, 0x00800000u, 0x00C00000u, 0x02000000u}) {
        for (size_t i = 0; i < std::max(singles.size(), doubles.size()); ++i) {
            const u32 single = singles[i % singles.size()];
            const u64 double_ = doubles[i % doubles.size()];
            const RoundResults expected = soft.Run(fpcr, single, double_);
            const RoundResults actual = inline_.Run(fpcr, single, double_);
            INFO("fpcr " << fpcr << " single " << single << " double " << double_);
            CHECK(actual.frinta_s == expected.frinta_s);
            CHECK(actual.frintx_s == expected.frintx_s);
            CHECK(actual.frinta_d == expected.frinta_d);
            CHECK(actual.frintx_d == expected.frintx_d);
            CHECK(actual.fpsr == expected.fpsr);
        }
    }

    // FRINTA rounds ties away from zero and keeps the sign of a zero; FRINTX reports inexact.
    const RoundResults r = inline_.Run(0, 0x40200000 /* 2.5 */, 0xBFE0000000000000 /* -0.5 */);
    CHECK(r.frinta_s == 0x40400000);  // 3.0
    CHECK(r.frintx_s == 0x40000000);  // 2.0
    CHECK(r.frinta_d == 0xBFF0000000000000);  // -1.0
    CHECK(r.frintx_d == 0x8000000000000000);  // -0.0
    CHECK((r.fpsr & 0x10) != 0);  // IXC
    const RoundResults z = inline_.Run(0, 0xBE99999A /* -0.3 */, 0x4008000000000000 /* 3.0 */);
    CHECK(z.frinta_s == 0x80000000);  // -0.0
    CHECK(z.frinta_d == 0x4008000000000000);
    CHECK(z.frintx_d == 0x4008000000000000);
}

TEST_CASE("A64: FRINTN, FRINTP, FRINTM, FRINTZ and FRINTI leave IXC clear", "[a64]") {
    HaltingEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    A64::Jit jit{conf};
    env.jit = &jit;

    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    code.FRINTN(S1, S0);
    code.FRINTP(S2, S0);
    code.FRINTM(D4, D3);
    code.FRINTZ(D5, D3);
    code.FRINTI(S6, S0);
    code.FRINTN(V7.S4(), V8.S4());
    code.BRK(0);

    jit.SetPC(0);
    jit.SetFpcr(0);
    jit.SetFpsr(0);
    jit.SetVector(0, {F(2.5f), 0});
    jit.SetVector(3, {D(-2.5), 0});
    jit.SetVector(8, {F(1.25f) | (F(-0.75f) << 32), F(3.5f) | (F(0.5f) << 32)});
    CheckedRun([&]() { jit.Run(); });

    CHECK(jit.GetVector(1)[0] == F(2.0f));
    CHECK(jit.GetVector(2)[0] == F(3.0f));
    CHECK(jit.GetVector(4)[0] == D(-3.0));
    CHECK(jit.GetVector(5)[0] == D(-2.0));
    CHECK(jit.GetVector(6)[0] == F(2.0f));
    CHECK(jit.GetVector(7) == Vector{F(1.0f) | (F(-1.0f) << 32), F(4.0f) | (F(0.0f) << 32)});
    CHECK((jit.GetFpsr() & (1 << 4)) == 0);
}
