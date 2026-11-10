// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "common/httplib.h"

#include <discord_rpc.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/file_sys/switch2_titles.h"
#include "qt_common/util/boxart.h"
#include "qt_common/util/title.h"

#include "discord_impl.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace DiscordRPC {

static constexpr char DEFAULT_DISCORD_TEXT[] = "Nemyu is an emulator for the Nintendo Switch 2";
// Discord caches external images by URL: bump ?v= whenever one of these files changes.
static constexpr char DEFAULT_DISCORD_IMAGE[] =
    "https://gitlab.com/nemyu-emulator/boxart/-/raw/main/img/nemyu.png?v=2";
// Not the logo, which is already the small image.
static constexpr char PLACEHOLDER_IMAGE[] =
    "https://gitlab.com/nemyu-emulator/boxart/-/raw/main/img/placeholder.png?v=2";

static s64 Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Status of a HEAD request for url, or 0 if the server could not be reached.
static int HeadStatus(const std::string& url, std::stop_token stop_token) {
    const auto scheme_end = url.find("://");
    const auto path_start =
        scheme_end == std::string::npos ? scheme_end : url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return 0;
    }

    try {
        httplib::Client client(url.substr(0, path_start));
        client.set_connection_timeout(3);
        client.set_read_timeout(3);
        client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
        client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

        const std::stop_callback cancel{stop_token, [&client] { client.stop(); }};
        const auto result = client.Head(url.substr(path_start));
        return result ? result->status : 0;
    } catch (const std::exception& e) {
        LOG_WARNING(Frontend, "Box art lookup for {} failed: {}", url, e.what());
        return 0;
    }
}

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    DiscordEventHandlers handlers{};
    // The number is the client ID for Nemyu, it's used for images and the
    // application name
    Discord_Initialize("1566252375490957322", &handlers, 1, nullptr);
    worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
}

DiscordImpl::~DiscordImpl() {
    // Waits for the worker so nothing is sent after the presence is cleared.
    worker.request_stop();
    worker.join();
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    Post({.state = State::Cleared});
}

void DiscordImpl::Update() {
    if (!system.IsPoweredOn()) {
        Post({.state = State::Idle, .start_time = Now()});
        return;
    }

    std::string title = QtCommon::Title::GetName(system.GetAppLoader());
    std::string slug = FileSys::GetCoverSlug(system.GetApplicationProcessProgramID(), title);
    std::vector<std::string> cover_urls;
    if (!slug.empty()) {
        cover_urls = QtCommon::Boxart::GetCoverUrls(slug);
    }

    Post({
        .state = State::InGame,
        .title = std::move(title),
        .cover_slug = std::move(slug),
        .cover_urls = std::move(cover_urls),
        .start_time = Now(),
    });
}

void DiscordImpl::Post(Request request) {
    {
        std::scoped_lock lock{mutex};
        pending = std::move(request);
    }
    request_cv.notify_one();
}

void DiscordImpl::Run(std::stop_token stop_token) {
    Common::SetCurrentThreadName("DiscordRPC");

    while (true) {
        Request request;
        {
            std::unique_lock lock{mutex};
            request_cv.wait(lock, stop_token, [this] { return pending.has_value(); });
            if (stop_token.stop_requested()) {
                return;
            }
            request = std::move(*pending);
            pending.reset();
        }

        if (request.state == State::Cleared) {
            Discord_ClearPresence();
            continue;
        }

        DiscordRichPresence presence{};
        presence.startTimestamp = request.start_time;

        if (request.state == State::Idle) {
            presence.largeImageKey = DEFAULT_DISCORD_IMAGE;
            presence.largeImageText = DEFAULT_DISCORD_TEXT;
            presence.status_display_type = DiscordStatusDisplayType_Name;
            presence.details = "Currently not in game";
            Discord_UpdatePresence(&presence);
            continue;
        }

        const std::string cover = FindCover(request, stop_token);
        {
            // The game may have been left during the lookup; the newer request is sent instead.
            std::scoped_lock lock{mutex};
            if (pending || stop_token.stop_requested()) {
                continue;
            }
        }

        presence.largeImageKey = cover.c_str();
        presence.largeImageText = request.title.c_str();
        presence.smallImageKey = DEFAULT_DISCORD_IMAGE;
        presence.smallImageText = DEFAULT_DISCORD_TEXT;
        presence.state = request.title.c_str();
        presence.status_display_type = DiscordStatusDisplayType_State;
        presence.details = "Currently in game";
        Discord_UpdatePresence(&presence);
    }
}

std::string DiscordImpl::FindCover(const Request& request, std::stop_token stop_token) {
    const auto& urls = request.cover_urls;
    if (urls.empty()) {
        return PLACEHOLDER_IMAGE;
    }
    if (const auto it = covers.find(urls.front()); it != covers.end()) {
        if (it->second != PLACEHOLDER_IMAGE) {
            // Retries the download if an earlier one failed, it is skipped once on disk.
            QtCommon::Boxart::Save(request.cover_slug, it->second);
        }
        return it->second;
    }

    bool all_missing = true;
    for (const auto& url : urls) {
        if (stop_token.stop_requested()) {
            return PLACEHOLDER_IMAGE;
        }
        const int status = HeadStatus(url, stop_token);
        if (status == 200) {
            covers.emplace(urls.front(), url);
            // Kept on disk for screens that cannot wait on the network, like the loading screen.
            QtCommon::Boxart::Save(request.cover_slug, url);
            return url;
        }
        all_missing = all_missing && status == 404;
    }

    // A failed request is retried on the next update instead of remembered as missing.
    if (all_missing) {
        covers.emplace(urls.front(), PLACEHOLDER_IMAGE);
    }
    return PLACEHOLDER_IMAGE;
}

} // namespace DiscordRPC
