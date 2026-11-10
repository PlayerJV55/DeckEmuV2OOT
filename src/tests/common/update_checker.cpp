// SPDX-FileCopyrightText: Copyright 2026 Nemyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Adapted from Lichii's updater tests (#82, #504, zortos293/Lichii) for Nemyu's GitLab releases.

#include <array>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "common/httplib.h"
#include "common/net/net.h"
#include "frontend_common/update_checker.h"

namespace {

constexpr std::string_view Downloads = "https://gitlab.com/nemyu-emulator/nemyu/-/releases/";

Common::Net::Release MakeRelease(std::string tag, bool with_package = true) {
    Common::Net::Release release{};
    release.tag = tag;
    release.title = "Nemyu " + tag;
    if (with_package) {
        const auto zip = "nemyu-" + tag + "-windows-x86_64.zip";
        release.assets = {std::string{Downloads} + tag + "/downloads/" + zip,
                          std::string{Downloads} + tag + "/downloads/" + zip + ".sha256"};
    }
    return release;
}

} // namespace

TEST_CASE("Nemyu release tags parse into versions", "[updater]") {
    const auto stable = UpdateChecker::ParseTag("nemyu-v0.1.7");
    REQUIRE(stable);
    CHECK(stable->major == 0);
    CHECK(stable->minor == 1);
    CHECK(stable->patch == 7);
    CHECK_FALSE(stable->experimental);

    const auto experimental = UpdateChecker::ParseTag("nemyu-v0.1.7-experimental.12");
    REQUIRE(experimental);
    CHECK(experimental->experimental == 12u);

    const std::string bad = GENERATE("", "v0.1.7", "nemyu-0.1.7", "nemyu-v0.1", "nemyu-v0.1.7.1",
                                     "nemyu-v0.1.7-experimental.", "nemyu-v0.1.7-experimental.0",
                                     "nemyu-v0.1.7-rc.1", "nemyu-v01.1.7", "nemyu-v0.1.+7",
                                     "nemyu-v0.1.7-experimental.4x", "5f1cd71adf-test/ports75",
                                     "nemyu-v0.1.99999999999");
    CHECK_FALSE(UpdateChecker::ParseTag(bad));
}

TEST_CASE("Experimental builds come before their normal release", "[updater]") {
    const auto v = [](const char* tag) { return *UpdateChecker::ParseTag(tag); };
    CHECK(v("nemyu-v0.1.7-experimental.3") < v("nemyu-v0.1.7-experimental.4"));
    CHECK(v("nemyu-v0.1.7-experimental.9") < v("nemyu-v0.1.7-experimental.10"));
    CHECK(v("nemyu-v0.1.7-experimental.40") < v("nemyu-v0.1.7"));
    CHECK(v("nemyu-v0.1.7") < v("nemyu-v0.1.8-experimental.1"));
    CHECK(v("nemyu-v0.1.9") < v("nemyu-v0.2.0"));
    CHECK(v("nemyu-v0.1.7") == v("nemyu-v0.1.7"));
}

TEST_CASE("Experimental builds are offered both channels, normal releases only theirs",
          "[updater]") {
    const auto releases = std::array{
        MakeRelease("nemyu-v0.1.8-experimental.1"),
        MakeRelease("nemyu-v0.1.7"),
        MakeRelease("nemyu-v0.1.7-experimental.4"),
        MakeRelease("nemyu-v0.1.6"),
    };
    const auto from_experimental =
        UpdateChecker::SelectUpdate(releases, "nemyu-v0.1.7-experimental.3", "windows-x86_64");
    REQUIRE(from_experimental);
    CHECK(from_experimental->tag == "nemyu-v0.1.8-experimental.1");

    const auto from_stable = UpdateChecker::SelectUpdate(releases, "nemyu-v0.1.6", "windows-x86_64");
    REQUIRE(from_stable);
    CHECK(from_stable->tag == "nemyu-v0.1.7");

    CHECK_FALSE(UpdateChecker::SelectUpdate(releases, "nemyu-v0.1.7", "windows-x86_64"));
    CHECK_FALSE(
        UpdateChecker::SelectUpdate(releases, "nemyu-v0.1.8-experimental.1", "windows-x86_64"));
    CHECK_FALSE(UpdateChecker::SelectUpdate(releases, "nemyu-v0.2.0", "windows-x86_64"));
    CHECK_FALSE(UpdateChecker::SelectUpdate({}, "nemyu-v0.1.6", "windows-x86_64"));
}

TEST_CASE("Local builds never select an update", "[updater]") {
    const auto releases = std::array{MakeRelease("nemyu-v0.1.7")};
    const std::string tag = GENERATE("", "5f1cd71adf-test/ports75", "nemyu-v0.1");
    CHECK_FALSE(UpdateChecker::SelectUpdate(releases, tag, "windows-x86_64"));
}

TEST_CASE("Releases without this platform's package are skipped unless the platform has none",
          "[updater]") {
    const auto releases = std::array{
        MakeRelease("nemyu-v0.1.7-experimental.5", false),
        MakeRelease("nemyu-v0.1.7-experimental.4"),
    };
    const auto windows =
        UpdateChecker::SelectUpdate(releases, "nemyu-v0.1.7-experimental.3", "windows-x86_64");
    REQUIRE(windows);
    CHECK(windows->tag == "nemyu-v0.1.7-experimental.4");

    const auto other = UpdateChecker::SelectUpdate(releases, "nemyu-v0.1.7-experimental.3", "");
    REQUIRE(other);
    CHECK(other->tag == "nemyu-v0.1.7-experimental.5");
}

TEST_CASE("The package and its checksum are matched by their exact GitLab URLs", "[updater]") {
    const auto release = MakeRelease("nemyu-v0.1.7-experimental.4");
    const auto package = UpdateChecker::FindPackage(release, "windows-x86_64");
    REQUIRE(package);
    CHECK(package->url == "https://gitlab.com");
    CHECK(package->path == "/nemyu-emulator/nemyu/-/releases/nemyu-v0.1.7-experimental.4/"
                           "downloads/nemyu-nemyu-v0.1.7-experimental.4-windows-x86_64.zip");
    CHECK(package->filename == "nemyu-nemyu-v0.1.7-experimental.4-windows-x86_64.zip");
    const auto checksum = UpdateChecker::FindChecksum(release, *package);
    REQUIRE(checksum);
    CHECK(checksum->filename == package->filename + ".sha256");
    CHECK_FALSE(UpdateChecker::FindPackage(release, ""));
    CHECK_FALSE(UpdateChecker::FindPackage(release, "linux-x86_64"));

    const std::string url = GENERATE(
        "https://gitlab.com/ericocristiam/nemyu-coco/-/releases/nemyu-v0.1.7-experimental.4/"
        "downloads/nemyu-nemyu-v0.1.7-experimental.4-windows-x86_64.zip",
        "https://gitlab.com.example.com/nemyu-emulator/nemyu/-/releases/"
        "nemyu-v0.1.7-experimental.4/downloads/nemyu-nemyu-v0.1.7-experimental.4-windows-x86_64.zip",
        "http://gitlab.com/nemyu-emulator/nemyu/-/releases/nemyu-v0.1.7-experimental.4/downloads/"
        "nemyu-nemyu-v0.1.7-experimental.4-windows-x86_64.zip",
        "https://gitlab.com/nemyu-emulator/nemyu/-/releases/nemyu-v0.1.7-experimental.3/downloads/"
        "nemyu-nemyu-v0.1.7-experimental.4-windows-x86_64.zip");
    auto lookalike = release;
    lookalike.assets = {url};
    CHECK_FALSE(UpdateChecker::FindPackage(lookalike, "windows-x86_64"));
}

TEST_CASE("Checksum files are read in sha256sum and bare form", "[updater]") {
    const std::string digest(64, 'A');
    const std::string lower(64, 'a');
    CHECK(UpdateChecker::ParseChecksum(digest + "  nemyu.zip\n", "nemyu.zip") == lower);
    CHECK(UpdateChecker::ParseChecksum(digest + " *nemyu.zip\r\n", "nemyu.zip") == lower);
    CHECK(UpdateChecker::ParseChecksum(digest + "\n", "nemyu.zip") == lower);
    CHECK_FALSE(UpdateChecker::ParseChecksum(digest + "  other.zip\n", "nemyu.zip"));
    CHECK_FALSE(UpdateChecker::ParseChecksum(std::string(63, 'a'), "nemyu.zip"));
    CHECK_FALSE(UpdateChecker::ParseChecksum(std::string(64, 'g'), "nemyu.zip"));
    CHECK_FALSE(UpdateChecker::ParseChecksum("", "nemyu.zip"));
}

TEST_CASE("GitLab release JSON is parsed", "[updater]") {
    const std::string_view json = R"([
        {"name":"Nemyu v0.1.7-experimental.3","tag_name":"nemyu-v0.1.7-experimental.3",
         "description":"Notes","released_at":"2026-11-07T21:50:16.610Z","upcoming_release":false,
         "assets":{"count":1,"sources":[{"format":"zip","url":"https://gitlab.com/x.zip"}],
                   "links":[{"name":"nemyu-nemyu-v0.1.7-experimental.3-windows-x86_64.zip",
                             "url":"https://gitlab.com/api/v4/projects/nemyu-emulator%2Fnemyu/packages/generic/x",
                             "direct_asset_url":"https://gitlab.com/nemyu-emulator/nemyu/-/releases/nemyu-v0.1.7-experimental.3/downloads/nemyu-nemyu-v0.1.7-experimental.3-windows-x86_64.zip",
                             "link_type":"other"}]},
         "_links":{"self":"https://gitlab.com/nemyu-emulator/nemyu/-/releases/nemyu-v0.1.7-experimental.3"}},
        {"name":null,"tag_name":"nemyu-v0.1.6","description":null},
        {"tag_name":"nemyu-v0.1.9","upcoming_release":true},
        {"name":"no tag"}
    ])";
    const auto releases = Common::Net::Release::ListFromGitLabJson(json);
    REQUIRE(releases.size() == 2);
    CHECK(releases[0].tag == "nemyu-v0.1.7-experimental.3");
    CHECK(releases[0].title == "Nemyu v0.1.7-experimental.3");
    CHECK(releases[0].body == "Notes");
    CHECK(releases[0].html_url ==
          "https://gitlab.com/nemyu-emulator/nemyu/-/releases/nemyu-v0.1.7-experimental.3");
    CHECK(UpdateChecker::FindPackage(releases[0], "windows-x86_64"));
    CHECK(releases[1].title == "nemyu-v0.1.6");
    CHECK(releases[1].body.empty());
    CHECK(releases[1].assets.empty());

    const std::string_view bad =
        GENERATE("", "not JSON", R"({"message":"404 Project Not Found"})", "null", "[]");
    CHECK(Common::Net::Release::ListFromGitLabJson(bad).empty());
}

TEST_CASE("Failed release requests are classified by status", "[updater]") {
    CHECK(UpdateChecker::ClassifyFailure(403) == UpdateChecker::Failure::RateLimited);
    CHECK(UpdateChecker::ClassifyFailure(429) == UpdateChecker::Failure::RateLimited);
    CHECK(UpdateChecker::ClassifyFailure(404) == UpdateChecker::Failure::Http);
    CHECK(UpdateChecker::ClassifyFailure(500) == UpdateChecker::Failure::Http);
    CHECK(UpdateChecker::ClassifyFailure(0) == UpdateChecker::Failure::Network);
}

TEST_CASE("Release requests report HTTP failures without throwing", "[updater]") {
    const int status = GENERATE(200, 404, 429, 500);
    httplib::Server server;
    server.Get("/releases", [status](const httplib::Request& request, httplib::Response& response) {
        response.status = request.get_header_value("User-Agent") == "Nemyu" ? status : 400;
        response.set_content("[]", "application/json");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::jthread listener([&server](std::stop_token stop) {
        const std::stop_callback stop_server(stop, [&server] { server.stop(); });
        server.listen_after_bind();
    });
    server.wait_until_ready();
    const auto response =
        Common::Net::Fetch("http://127.0.0.1:" + std::to_string(port), "/releases");
    CHECK(response.status == status);
    CHECK(response.body.has_value() == (status == 200));
}
