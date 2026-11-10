// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_profiles.h"
#include "common/nx2_switches.h"
#include "common/nx2_title_defaults.h"

namespace Common {
namespace {

constexpr const char* SWITCH_A = "EDEN_NX2_TEST_PROFILE_A";
constexpr const char* SWITCH_B = "EDEN_NX2_TEST_PROFILE_B";
constexpr const char* SWITCH_C = "EDEN_NX2_TEST_PROFILE_C";
// Read once per emulation session, so a profile may change it between games
constexpr const char* SESSION_SWITCH = "EDEN_NX2_FLUSH_DRAWS";

void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

std::optional<std::string> Variable(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

void ClearTestSwitches() {
    RestoreNx2Profile();
    for (const char* name : {SWITCH_A, SWITCH_B, SWITCH_C}) {
        SetVariable(name, nullptr);
    }
}

} // Anonymous namespace

TEST_CASE("Nx2Profiles: profile names", "[common]") {
    CHECK(IsValidNx2ProfileName("OoT"));
    CHECK(IsValidNx2ProfileName("Mario Kart World (AMD) v2"));
    CHECK(IsValidNx2ProfileName("a\xC3\xA7\xC3\xA3o"));
    CHECK(IsValidNx2ProfileName("CONSOLE"));
    CHECK(IsValidNx2ProfileName("COM10"));
    CHECK(IsValidNx2ProfileName("v1.2"));
    CHECK(IsValidNx2ProfileName(std::string(64, 'a')));

    CHECK_FALSE(IsValidNx2ProfileName(""));
    CHECK_FALSE(IsValidNx2ProfileName(std::string(65, 'a')));
    CHECK_FALSE(IsValidNx2ProfileName("."));
    CHECK_FALSE(IsValidNx2ProfileName(".."));
    CHECK_FALSE(IsValidNx2ProfileName("name."));
    CHECK_FALSE(IsValidNx2ProfileName(" name"));
    CHECK_FALSE(IsValidNx2ProfileName("name "));
    for (const char* name : {"a/b", "a\\b", "a:b", "a*b", "a?b", "a|b", "a\"b", "a<b", "a>b",
                             "tab\there", "line\nbreak"}) {
        CHECK_FALSE(IsValidNx2ProfileName(name));
    }
    for (const char* name : {"CON", "con", "Prn", "aux", "NUL", "com1", "LPT9", "nul.txt",
                             "AUX .profile"}) {
        CHECK_FALSE(IsValidNx2ProfileName(name));
    }
}

TEST_CASE("Nx2Profiles: the real environment wins over a profile", "[common]") {
    ClearTestSwitches();
    SetVariable(SWITCH_C, "environment");

    const std::vector<Nx2Switch> profile{
        {SWITCH_C, "profile"},
        {SWITCH_A, "1"},
        {SWITCH_B, "x"},
        {SWITCH_A, "2"},
    };
    const std::vector<Nx2Switch> effective = EffectiveNx2ProfileSwitches(profile);
    REQUIRE(effective.size() == 2);
    CHECK(effective[0].name == SWITCH_A);
    CHECK(effective[0].value == "2");
    CHECK(effective[1].name == SWITCH_B);
    CHECK(effective[1].value == "x");

    ClearTestSwitches();
}

TEST_CASE("Nx2Profiles: applying and restoring a profile", "[common]") {
    ClearTestSwitches();
    // Stands for a value from the switch files
    SetVariable(SWITCH_A, "global");
    const u32 generation = Nx2::g_program_generation.load();

    const std::vector<Nx2Switch> profile{{SWITCH_A, "profile"}, {SWITCH_B, "on"}};
    ApplyNx2Profile("test", profile);
    CHECK(Variable(SWITCH_A) == "profile");
    CHECK(Variable(SWITCH_B) == "on");
    CHECK(Nx2::g_program_generation.load() != generation);
    CHECK_FALSE(IsSetInRealEnvironment(SWITCH_B));

    // A second profile replaces the first instead of stacking on it
    const std::vector<Nx2Switch> other{{SWITCH_C, "3"}};
    ApplyNx2Profile("other", other);
    CHECK(Variable(SWITCH_A) == "global");
    CHECK(Variable(SWITCH_B) == std::nullopt);
    CHECK(Variable(SWITCH_C) == "3");

    RestoreNx2Profile();
    CHECK(Variable(SWITCH_A) == "global");
    CHECK(Variable(SWITCH_B) == std::nullopt);
    CHECK(Variable(SWITCH_C) == std::nullopt);

    // Nothing to undo
    RestoreNx2Profile();
    CHECK(Variable(SWITCH_A) == "global");

    ClearTestSwitches();
}

TEST_CASE("Nx2Profiles: a switch set twice is restored to its first value", "[common]") {
    ClearTestSwitches();
    SetVariable(SWITCH_A, "global");
    const std::vector<Nx2Switch> profile{{SWITCH_A, "1"}, {SWITCH_A, "2"}};
    ApplyNx2Profile("twice", profile);
    CHECK(Variable(SWITCH_A) == "2");
    RestoreNx2Profile();
    CHECK(Variable(SWITCH_A) == "global");
    ClearTestSwitches();
}

TEST_CASE("Nx2Profiles: switches read once per process", "[common]") {
    ClearTestSwitches();
    SetVariable(SWITCH_B, "global");

    const std::vector<Nx2Switch> none;
    const std::vector<Nx2Switch> sets_a{{SWITCH_A, "1"}, {SESSION_SWITCH, "128"}};
    const std::vector<Nx2Switch> sets_a_too{{SWITCH_A, "1"}};
    const std::vector<Nx2Switch> sets_b_as_global{{SWITCH_B, "global"}};
    const std::vector<Nx2Switch> sets_b{{SWITCH_B, "other"}};
    const std::vector<Nx2Switch> sets_session{{SESSION_SWITCH, "64"}};

    CHECK(Nx2ProcessSwitchChanges(none, none).empty());
    CHECK(Nx2ProcessSwitchChanges(sets_a, sets_a_too).empty());
    CHECK(Nx2ProcessSwitchChanges(none, sets_session).empty());
    CHECK(Nx2ProcessSwitchChanges(none, sets_b_as_global).empty());
    CHECK(Nx2ProcessSwitchChanges(sets_a, none) == std::vector<std::string>{SWITCH_A});
    CHECK(Nx2ProcessSwitchChanges(none, sets_a) == std::vector<std::string>{SWITCH_A});
    CHECK(Nx2ProcessSwitchChanges(sets_a, sets_b) ==
          std::vector<std::string>{SWITCH_A, SWITCH_B});

    ClearTestSwitches();
}

} // namespace Common
