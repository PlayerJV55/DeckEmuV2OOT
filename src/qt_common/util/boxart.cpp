// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/httplib.h"

#include <QObject>
#include <QString>
#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/file_sys/switch2_titles.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/boxart.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

namespace QtCommon::Boxart {

namespace {

// Eden's box art, which covers Switch 1 games.
constexpr char EDEN_BOXART_URL[] =
    "https://raw.githubusercontent.com/eden-emulator/boxart/refs/heads/master/img/";

constexpr std::size_t MaxQueued = 32;
constexpr std::size_t MaxCoverSize = 8 * 1024 * 1024;

bool IsValidSlug(std::string_view slug) {
    // The repositories also hold the logo and the placeholder, which are not box art.
    if (slug.empty() || slug == "placeholder" || slug == "nemyu") {
        return false;
    }
    return std::all_of(slug.begin(), slug.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}

bool IsImage(std::string_view data) {
    constexpr std::string_view Png{"\x89PNG\r\n\x1a\n", 8};
    constexpr std::string_view Jpeg{"\xFF\xD8\xFF", 3};
    return data.starts_with(Png) || data.starts_with(Jpeg);
}

// Fetches url into out. Returns the HTTP status, or 0 if the server could not be reached or did
// not send a PNG or JPEG image.
int Download(const std::string& url, std::string& out, std::stop_token stop_token) {
    const auto scheme_end = url.find("://");
    const auto path_start =
        scheme_end == std::string::npos ? scheme_end : url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return 0;
    }

    try {
        httplib::Client client(url.substr(0, path_start));
        client.set_connection_timeout(3);
        client.set_read_timeout(5);
        client.set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
        client.load_ca_cert_store(kCert, sizeof(kCert));
#endif

        const std::stop_callback cancel{stop_token, [&client] { client.stop(); }};
        int status = 0;
        const auto result = client.Get(
            url.substr(path_start),
            [&status](const httplib::Response& response) {
                status = response.status;
                if (status != 200) {
                    return true;
                }
                const std::string type = response.get_header_value("Content-Type");
                return (type.starts_with("image/png") || type.starts_with("image/jpeg")) &&
                       response.get_header_value_u64("Content-Length") <= MaxCoverSize;
            },
            [&](const char* data, std::size_t length) {
                if (status != 200) {
                    return true;
                }
                if (stop_token.stop_requested() || out.size() + length > MaxCoverSize) {
                    return false;
                }
                out.append(data, length);
                return true;
            });
        if (!result || (result->status == 200 && !IsImage(out))) {
            return 0;
        }
        return result->status;
    } catch (const std::exception& e) {
        LOG_WARNING(Frontend, "Box art download from {} failed: {}", url, e.what());
        return 0;
    }
}

bool Store(const std::filesystem::path& path, std::string_view data) {
    // Written next to its final name and renamed, so a partial file is never taken for box art.
    const auto temp = std::filesystem::path{path}.concat(".part");
    if (!Common::FS::CreateDirs(path.parent_path()) ||
        Common::FS::WriteStringToFile(temp, Common::FS::FileType::BinaryFile, data) !=
            data.size() ||
        !Common::FS::RenameFile(temp, path)) {
        Common::FS::RemoveFile(temp);
        return false;
    }
    return true;
}

class Downloader {
public:
    static Downloader& Instance() {
        static Downloader instance;
        return instance;
    }

    void Push(std::string slug, std::vector<std::string> urls, bool urgent) {
        if (!IsValidSlug(slug) || Common::FS::IsFile(GetCachePath(slug))) {
            return;
        }
        {
            std::scoped_lock lock{mutex};
            if (stopped) {
                return;
            }
            if (urgent) {
                const auto queued = std::find_if(queue.begin(), queue.end(),
                                                 [&](const Job& job) { return job.slug == slug; });
                if (queued != queue.end()) {
                    std::rotate(queue.begin(), queued, std::next(queued));
                    return;
                }
                if (!seen.insert(slug).second) {
                    return;
                }
                queue.push_front({std::move(slug), std::move(urls)});
            } else {
                // A full queue drops the request, the next game list refresh asks again.
                if (queue.size() >= MaxQueued || !seen.insert(slug).second) {
                    return;
                }
                queue.push_back({std::move(slug), std::move(urls)});
            }
            if (!worker.joinable()) {
                worker = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
            }
        }
        request_cv.notify_one();
    }

    void Stop() {
        std::jthread stopping;
        {
            std::scoped_lock lock{mutex};
            stopped = true;
            queue.clear();
            stopping = std::move(worker);
        }
        // Joined outside the lock, as the worker takes it to see the stop request.
    }

    void SetOnSaved(QObject* context, std::function<void(const std::string&)> callback) {
        std::scoped_lock lock{mutex};
        if (callback) {
            listeners.insert_or_assign(context, std::move(callback));
        } else {
            listeners.erase(context);
        }
    }

private:
    struct Job {
        std::string slug;
        std::vector<std::string> urls; ///< Tried in order
    };

    void Run(std::stop_token stop_token) {
        Common::SetCurrentThreadName("BoxartDownload");

        while (true) {
            Job job;
            {
                std::unique_lock lock{mutex};
                request_cv.wait(lock, stop_token, [this] { return !queue.empty(); });
                if (stop_token.stop_requested()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }

            if (!Fetch(job, stop_token)) {
                std::scoped_lock lock{mutex};
                seen.erase(job.slug);
            }
        }
    }

    // False if the box art may still exist, so a later request should try again.
    bool Fetch(const Job& job, std::stop_token stop_token) {
        const auto path = GetCachePath(job.slug);
        if (Common::FS::IsFile(path)) {
            return true;
        }

        bool all_missing = true;
        for (const auto& url : job.urls) {
            if (stop_token.stop_requested()) {
                return false;
            }
            std::string data;
            const int status = Download(url, data, stop_token);
            if (status == 200) {
                if (!Store(path, data)) {
                    LOG_WARNING(Frontend, "Could not save box art to {}",
                                Common::FS::PathToUTF8String(path));
                    return false;
                }
                LOG_INFO(Frontend, "Saved box art from {}", url);
                Notify(job.slug);
                return true;
            }
            all_missing = all_missing && status == 404;
        }
        return all_missing;
    }

    // Posted under the lock, so a listener that was just removed is never called.
    void Notify(const std::string& slug) {
        std::scoped_lock lock{mutex};
        for (const auto& [context, callback] : listeners) {
            QMetaObject::invokeMethod(
                context, [callback, slug] { callback(slug); }, Qt::QueuedConnection);
        }
    }

    std::mutex mutex;
    std::condition_variable_any request_cv;
    std::deque<Job> queue;
    std::unordered_set<std::string> seen; ///< Queued, saved or missing everywhere this session
    bool stopped = false;
    std::unordered_map<QObject*, std::function<void(const std::string&)>> listeners;
    std::jthread worker;
};

} // namespace

std::filesystem::path GetCachePath(std::string_view slug) {
    if (!IsValidSlug(slug)) {
        return {};
    }
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "boxart" /
           fmt::format("{}.png", slug);
}

std::vector<std::string> GetCoverUrls(std::string_view slug) {
    std::vector<std::string> urls;
    std::string base_url = UISettings::values.discord_boxart_url.GetValue();
    if (!base_url.empty()) {
        if (!base_url.ends_with('/')) {
            base_url += '/';
        }
        urls.push_back(fmt::format("{}{}.png", base_url, slug));
    }
    urls.push_back(fmt::format("{}{}.png", EDEN_BOXART_URL, slug));
    return urls;
}

QImage LoadCachedCover(u64 program_id, const std::string& title) {
    const auto path = GetCachePath(FileSys::GetCoverSlug(program_id, title));
    if (path.empty() || !Common::FS::IsFile(path)) {
        return {};
    }
    return QImage(QString::fromStdString(Common::FS::PathToUTF8String(path)));
}

std::string GetSlug(u64 program_id, const std::string& title) {
    std::string slug = FileSys::GetCoverSlug(program_id, title);
    return IsValidSlug(slug) ? slug : std::string{};
}

void Prefetch(u64 program_id, const std::string& title, bool urgent) {
    // Only games the repositories may have: Switch 1 ones, and Switch 2 ones the title lists know.
    const u64 platform = program_id >> 56;
    if (platform != 0x01 && (platform != 0x04 || !FileSys::FindSwitch2Title(program_id))) {
        return;
    }
    std::string slug = FileSys::GetCoverSlug(program_id, title);
    std::vector<std::string> urls = GetCoverUrls(slug);
    Downloader::Instance().Push(std::move(slug), std::move(urls), urgent);
}

void Save(std::string slug, std::string url) {
    Downloader::Instance().Push(std::move(slug), {std::move(url)}, false);
}

void OnSaved(QObject* context, std::function<void(const std::string& slug)> on_saved) {
    Downloader::Instance().SetOnSaved(context, std::move(on_saved));
}

void Shutdown() {
    Downloader::Instance().Stop();
}

} // namespace QtCommon::Boxart
