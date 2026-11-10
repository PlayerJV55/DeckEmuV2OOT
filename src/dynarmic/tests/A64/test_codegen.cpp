// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Code generation shortcuts of the x64 backend and the IR passes, checked against the
// architecture. Each test runs whole blocks (a BRK at the end halts) with the default
// configuration and with the code generation options Nemyu turns on.

#include <array>
#include <bit>
#include <cstdint>

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

A64::UserConfig CodegenConfig(HaltingEnv& env, bool nemyu_codegen) {
    A64::UserConfig conf{};
    conf.callbacks = &env;
    conf.enable_cycle_counting = false;
    if (nemyu_codegen) {
        conf.spill_to_free_gpr = true;
        conf.inline_fp_round = true;
        conf.scratch_copy_to_free_reg = true;
        conf.elide_redundant_zext = true;
        conf.scalars_in_xmm = true;
        conf.compact_block_exits = true;
        conf.omit_block_frame = true;
        conf.halt_check_on_back_edges = true;
        conf.rsb_size = 32;
        conf.fast_dispatch_table_bits = 16;
        conf.fast_dispatch_set_associative = true;
        conf.reuse_host_flags = true;
        conf.inline_return_dispatch = true;
        conf.indirect_branch_cache = true;
    }
    return conf;
}

}  // namespace

TEST_CASE("A64: a W register read back as X is zero-extended", "[a64]") {
    for (const bool nemyu_codegen : {false, true}) {
        HaltingEnv env;
        A64::Jit jit{CodegenConfig(env, nemyu_codegen)};
        env.jit = &jit;

        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        code.LDRB(W1, X0);        // the test memory holds each address's low byte
        code.LSR(X2, X1, 2);      // read as X right after the W write
        code.ADD(W3, W4, W5);     // wraps at 32 bits
        code.ADD(X6, X3, X7);     // X3's upper half is zero
        code.LDR(W8, X0, 4);
        code.ADD(X9, X8, X8);
        code.ORR(W10, WZR, W11);  // MOV W10, W11 of a register with an upper half
        code.ADD(X12, X10, X10);
        code.ADD(W13, W11, 1);    // W13 = W11 + 1 while X11 is still needed
        code.ADD(X14, X11, X13);
        code.MOVZ(W15, 0xffff);   // an immediate written as W, read as X
        code.ADD(X16, X15, X15);
        code.BRK(0);

        jit.SetPC(0);
        jit.SetRegister(0, 0x1234);
        jit.SetRegister(4, 0xffffffffffffffff);
        jit.SetRegister(5, 2);
        jit.SetRegister(7, 0x100000000);
        jit.SetRegister(11, 0xabcdef0012345678);
        jit.SetRegister(15, 0xffffffff00000000);
        CheckedRun([&]() { jit.Run(); });

        INFO("nemyu codegen " << nemyu_codegen);
        CHECK(jit.GetRegister(1) == 0x34);
        CHECK(jit.GetRegister(2) == 0x34 >> 2);
        CHECK(jit.GetRegister(3) == 1);
        CHECK(jit.GetRegister(6) == 0x100000001);
        CHECK(jit.GetRegister(8) == 0x3b3a3938);
        CHECK(jit.GetRegister(9) == 2 * u64(0x3b3a3938));
        CHECK(jit.GetRegister(10) == 0x12345678);
        CHECK(jit.GetRegister(12) == 2 * u64(0x12345678));
        CHECK(jit.GetRegister(13) == 0x12345679);
        CHECK(jit.GetRegister(14) == 0xabcdef0012345678 + 0x12345679);
        CHECK(jit.GetRegister(15) == 0xffff);
        CHECK(jit.GetRegister(16) == 2 * 0xffff);
    }
}

TEST_CASE("A64: 64-bit add, subtract and compare with an immediate, and adds of live values", "[a64]") {
    for (const bool nemyu_codegen : {false, true}) {
        HaltingEnv env;
        A64::Jit jit{CodegenConfig(env, nemyu_codegen)};
        env.jit = &jit;

        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        code.SUBS(X1, X0, 0x1f);
        code.CSET(X2, oaknut::Cond::HI);
        code.ADDS(X3, X0, 0xfff000);
        code.CSET(X4, oaknut::Cond::CS);
        code.CMN(X0, 1);
        code.CSET(X5, oaknut::Cond::EQ);
        code.ADD(X6, X0, 8);
        code.ADD(X7, X0, X6);  // X0 and X6 both still needed below
        code.ADD(X8, X6, X0);
        code.ADD(W9, W0, 0x10);  // X0 still needed
        code.SUB(X10, X0, 0xfff000);
        code.CMP(X0, 0x20);
        code.CSET(X11, oaknut::Cond::LT);
        code.BRK(0);

        for (const u64 x0 : {u64(0), u64(0x1f), u64(0x20), u64(0xffffffffffffffff), u64(0xfffffffffff00000), u64(0x8000000000000000)}) {
            jit.SetPC(0);
            jit.SetRegister(0, x0);
            CheckedRun([&]() { jit.Run(); });
            INFO("x0 " << x0 << " nemyu codegen " << nemyu_codegen);
            CHECK(jit.GetRegister(0) == x0);
            CHECK(jit.GetRegister(1) == x0 - 0x1f);
            CHECK(jit.GetRegister(2) == (x0 > 0x1f ? 1 : 0));
            CHECK(jit.GetRegister(3) == x0 + 0xfff000);
            CHECK(jit.GetRegister(4) == (x0 + 0xfff000 < x0 ? 1 : 0));
            CHECK(jit.GetRegister(5) == (x0 + 1 == 0 ? 1 : 0));
            CHECK(jit.GetRegister(6) == x0 + 8);
            CHECK(jit.GetRegister(7) == x0 + x0 + 8);
            CHECK(jit.GetRegister(8) == x0 + x0 + 8);
            CHECK(jit.GetRegister(9) == u32(x0 + 0x10));
            CHECK(jit.GetRegister(10) == x0 - 0xfff000);
            CHECK(jit.GetRegister(11) == (s64(x0) < 0x20 ? 1 : 0));
        }
    }
}

TEST_CASE("A64: calls, returns and indirect branches through the return stack buffer and fast dispatch", "[a64]") {
    for (const bool nemyu_codegen : {false, true}) {
        HaltingEnv env;
        A64::Jit jit{CodegenConfig(env, nemyu_codegen)};
        env.jit = &jit;

        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        oaknut::Label add_one, double_it, nested, done;
        code.MOVZ(X0, 5);
        code.BL(add_one);      // pushes the return onto the RSB
        code.BL(double_it);
        code.MOVZ(X3, 6 * 4);  // the address of done
        code.BR(X3);           // fast dispatch
        code.MOVZ(X0, 0);      // skipped
        code.l(done);
        code.ADD(X1, X0, 100);
        code.BRK(0);
        code.l(add_one);
        code.ADD(X0, X0, 1);
        code.RET();
        code.l(double_it);
        code.MOV(X20, X30);
        code.BL(nested);       // a return to a return
        code.ADD(X0, X0, X0);
        code.MOV(X30, X20);
        code.RET();
        code.l(nested);
        code.ADD(X0, X0, 0);
        code.RET();

        for (int run = 0; run < 3; ++run) {  // later runs find every block and RSB entry compiled
            jit.SetPC(0);
            jit.SetRegister(0, 0);
            jit.SetRegister(1, 0);
            CheckedRun([&]() { jit.Run(); });
            INFO("run " << run << " nemyu codegen " << nemyu_codegen);
            CHECK(jit.GetRegister(0) == 12);
            CHECK(jit.GetRegister(1) == 112);
        }
    }
}

TEST_CASE("A64: an indirect branch reaches recompiled code after an invalidation", "[a64]") {
    for (const bool nemyu_codegen : {false, true}) {
        HaltingEnv env;
        A64::Jit jit{CodegenConfig(env, nemyu_codegen)};
        env.jit = &jit;

        env.code_mem.emplace_back(0xd2800100);  // MOV X0, 8
        env.code_mem.emplace_back(0xd61f0000);  // BR X0
        env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
        env.code_mem.emplace_back(0xd4200000);  // BRK #0

        // The first run compiles, the second fills the branch's own entry, the third uses it.
        for (int run = 0; run < 3; ++run) {
            jit.SetPC(0);
            CheckedRun([&]() { jit.Run(); });
            INFO("run " << run << " nemyu codegen " << nemyu_codegen);
            CHECK(jit.GetRegister(0) == 42);
        }

        env.code_mem[2] = 0xd28008a0;  // MOV X0, 69
        jit.InvalidateCacheRange(8, 4);
        for (int run = 0; run < 3; ++run) {
            jit.SetPC(0);
            CheckedRun([&]() { jit.Run(); });
            INFO("after the invalidation, run " << run << " nemyu codegen " << nemyu_codegen);
            CHECK(jit.GetRegister(0) == 69);
        }
    }
}

TEST_CASE("A64: a return in a block that sets the FPCR", "[a64]") {
    for (const bool nemyu_codegen : {false, true}) {
        HaltingEnv env;
        A64::Jit jit{CodegenConfig(env, nemyu_codegen)};
        env.jit = &jit;

        // The return target runs with the new FPCR (round towards zero): its location differs
        // from the one the returning block would build from its own.
        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        oaknut::Label set_rz;
        code.BL(set_rz);
        code.FADD(S2, S0, S1);
        code.BRK(0);
        code.l(set_rz);
        code.MSR(oaknut::SystemReg::FPCR, X5);
        code.RET();

        for (int run = 0; run < 2; ++run) {
            jit.SetPC(0);
            jit.SetFpcr(0);
            jit.SetRegister(5, 0x00C00000);  // RMode = 0b11, round towards zero
            jit.SetVector(0, {std::bit_cast<u32>(1.0f), 0});
            jit.SetVector(1, {std::bit_cast<u32>(0x1.8p-24f), 0});  // 0.75 ulp of 1.0: nearest rounds up
            CheckedRun([&]() { jit.Run(); });
            INFO("run " << run << " nemyu codegen " << nemyu_codegen);
            CHECK(jit.GetVector(2)[0] == std::bit_cast<u32>(1.0f));
            CHECK(jit.GetFpcr() == 0x00C00000);
        }
    }
}

TEST_CASE("A64: returns deeper than the return stack buffer and returns it did not predict", "[a64]") {
    for (const bool nemyu_codegen : {false, true}) {
        HaltingEnv env;
        A64::Jit jit{CodegenConfig(env, nemyu_codegen)};
        env.jit = &jit;

        // count recurses 40 deep, past the 8 or 32 entries of the return stack buffer, so the
        // outer returns find their entries overwritten. skip returns one instruction past where
        // its call pushed, so its entry does not match and the return goes to fast dispatch.
        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        oaknut::Label count, base, skip, done;
        code.MOVZ(X0, 0);
        code.MOVZ(X1, 40);
        code.BL(count);
        code.BL(skip);
        code.MOVZ(X0, 0);  // skipped
        code.l(done);
        code.ADD(X2, X0, 1);
        code.BRK(0);
        code.l(count);
        code.CBZ(X1, base);
        code.STP(X29, X30, SP, PRE_INDEXED, -16);
        code.SUB(X1, X1, 1);
        code.BL(count);
        code.ADD(X0, X0, 1);
        code.LDP(X29, X30, SP, POST_INDEXED, 16);
        code.l(base);
        code.RET();
        code.l(skip);
        code.ADD(X30, X30, 4);
        code.RET();

        for (int run = 0; run < 3; ++run) {  // later runs find every block and RSB entry compiled
            jit.SetPC(0);
            jit.SetSP(0x10000);
            jit.SetRegister(2, 0);
            CheckedRun([&]() { jit.Run(); });
            INFO("run " << run << " nemyu codegen " << nemyu_codegen);
            CHECK(jit.GetRegister(0) == 40);
            CHECK(jit.GetRegister(1) == 0);
            CHECK(jit.GetRegister(2) == 41);
            CHECK(jit.GetSP() == 0x10000);
        }
    }
}

TEST_CASE("A64: UBFM as a rotate and a low mask is a logical shift right", "[a64]") {
    for (const bool nemyu_codegen : {false, true}) {
        HaltingEnv env;
        A64::Jit jit{CodegenConfig(env, nemyu_codegen)};
        env.jit = &jit;

        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        code.LSR(X9, X0, 1);
        code.LSR(W10, W0, 31);
        code.UBFX(X11, X0, 4, 60);  // LSR #4
        code.UBFX(X12, X0, 4, 59);  // not a plain shift
        code.UBFX(W13, W0, 3, 29);  // LSR #3
        code.UBFX(W14, W0, 3, 28);  // not a plain shift
        code.LSR(X15, X0, 63);
        code.BRK(0);

        for (const u64 x0 : {u64(0), u64(0x1f), u64(0xffffffffffffffff), u64(0x8000000080000000), u64(0x0123456789abcdef)}) {
            jit.SetPC(0);
            jit.SetRegister(0, x0);
            CheckedRun([&]() { jit.Run(); });
            INFO("x0 " << x0 << " nemyu codegen " << nemyu_codegen);
            CHECK(jit.GetRegister(9) == x0 >> 1);
            CHECK(jit.GetRegister(10) == u32(x0) >> 31);
            CHECK(jit.GetRegister(11) == x0 >> 4);
            CHECK(jit.GetRegister(12) == ((x0 >> 4) & ((u64(1) << 59) - 1)));
            CHECK(jit.GetRegister(13) == u32(x0) >> 3);
            CHECK(jit.GetRegister(14) == ((u32(x0) >> 3) & ((u32(1) << 28) - 1)));
            CHECK(jit.GetRegister(15) == x0 >> 63);
        }
    }
}
