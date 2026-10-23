#include "release_feed.h"

#include <ctime>
#include <fstream>
#include <sstream>

namespace launcher {
namespace {

// 2: a release line may end with the symbols package. A cache of version 1 is dropped, so the next refresh asks GitHub
// without an ETag and finds the symbols of releases it listed before.
constexpr std::string_view kCacheHeader = "wind-launcher-sdk-cache 2";
constexpr std::string_view kApiHost = "https://api.github.com/";

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
}

bool has_tab_or_newline(std::string_view text) {
    return text.find_first_of("\t\r\n") != std::string_view::npos;
}

}

std::string today_utc() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char text[16];
    std::strftime(text, sizeof text, "%Y-%m-%d", &parts);
    return text;
}

std::string format_release_cache(const ReleaseCache& cache, const Platform& platform) {
    std::ostringstream out;
    out << kCacheHeader << '\n';
    out << "etag\t" << cache.etag << '\n';
    out << "fetched\t" << cache.fetched << '\n';
    out << "platform\t" << platform.os << '\t' << platform.arch << '\n';
    for (const SdkRelease& release : cache.releases) {
        if (has_tab_or_newline(release.version + release.date + release.archive_name + release.archive_url +
                    release.checksum_url)) {
            continue;
        }
        // A symbols package with a tab in it is left out; the SDK is still listed.
        const bool symbols = release.symbols &&
                             !has_tab_or_newline(release.symbols->name + release.symbols->url +
                                     release.symbols->checksum_url);
        out << "release\t" << release.version << '\t' << release.date << '\t' << release.size << '\t'
            << release.archive_name << '\t' << release.archive_url << '\t' << release.checksum_url;
        if (symbols) {
            out << '\t' << release.symbols->name << '\t' << release.symbols->url << '\t'
                << release.symbols->checksum_url << '\t' << release.symbols->size;
        }
        out << '\n';
    }
    return out.str();
}

ReleaseCache parse_release_cache(std::string_view text, const Platform& platform) {
    ReleaseCache cache;
    bool header = false;
    bool platform_ok = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (!header) {
            if (line != kCacheHeader) {
                return {};
            }
            header = true;
            continue;
        }
        const std::vector<std::string> fields = split(line, '\t');
        if (fields[0] == "etag" && fields.size() == 2) {
            cache.etag = fields[1];
        } else if (fields[0] == "fetched" && fields.size() == 2) {
            cache.fetched = fields[1];
        } else if (fields[0] == "platform" && fields.size() == 3) {
            platform_ok = fields[1] == platform.os && fields[2] == platform.arch;
        } else if (fields[0] == "release" && (fields.size() == 7 || fields.size() == 11)) {
            const std::optional<engine::EngineVersion> parsed = engine::parse_engine_version(fields[1]);
            if (!parsed || !fields[5].starts_with("https://") || !fields[6].starts_with("https://")) {
                return {};
            }
            SdkRelease release;
            release.version = fields[1];
            release.parsed = *parsed;
            release.date = fields[2];
            try {
                release.size = std::stoull(fields[3]);
            } catch (const std::exception&) {
                return {};
            }
            release.archive_name = fields[4];
            release.archive_url = fields[5];
            release.checksum_url = fields[6];
            if (fields.size() == 11) {
                if (!fields[8].starts_with("https://") || !fields[9].starts_with("https://")) {
                    return {};
                }
                SdkPackage symbols{.name = fields[7], .url = fields[8], .checksum_url = fields[9]};
                try {
                    symbols.size = std::stoull(fields[10]);
                } catch (const std::exception&) {
                    return {};
                }
                release.symbols = std::move(symbols);
            }
            cache.releases.push_back(std::move(release));
        } else {
            return {};
        }
    }
    if (!header || !platform_ok) {
        return {};
    }
    return cache;
}

ReleaseCache load_release_cache(const std::filesystem::path& file, const Platform& platform) {
    return parse_release_cache(read_text_file(file), platform);
}

bool save_release_cache(const std::filesystem::path& file, const ReleaseCache& cache, const Platform& platform) {
    if (file.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // Written beside and renamed, so a crash leaves the old cache.
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << format_release_cache(cache, platform);
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(temporary, file, error);
    return !error;
}

ReleaseFeed::ReleaseFeed(IDownloader& downloader, ReleaseFeedOptions options)
    : downloader_(&downloader)
    , options_(std::move(options)) {}

void ReleaseFeed::load_cache() {
    if (options_.cache_file.empty()) {
        return;
    }
    cache_ = load_release_cache(options_.cache_file, options_.platform);
    cache_.releases = sort_releases(std::move(cache_.releases));
    ++revision_;
}

void ReleaseFeed::refresh() {
    if (loading_) {
        return;
    }
    std::error_code error;
    if (!options_.work_directory.empty()) {
        std::filesystem::create_directories(options_.work_directory, error);
    }
    body_file_ = options_.work_directory / "releases-page.json";
    header_file_ = options_.work_directory / "releases-page.headers";
    loading_ = true;
    page_ = 0;
    seen_ = 0;
    etag_.clear();
    collected_.clear();
    ++revision_;
    request_page("https://api.github.com/repos/" + options_.repository + "/releases?per_page=100");
}

void ReleaseFeed::request_page(const std::string& url) {
    DownloadRequest request;
    request.url = url;
    request.output = body_file_;
    request.header_output = header_file_;
    request.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"};
    request.bearer_token = options_.token;
    request.download = false;
    // Only the first page has an ETag worth sending: a 304 keeps the whole cached list.
    if (page_ == 0 && !cache_.etag.empty() && !cache_.releases.empty()) {
        request.headers.push_back("If-None-Match: " + cache_.etag);
    }
    downloader_->start(std::move(request));
}

void ReleaseFeed::poll() {
    if (!loading_) {
        return;
    }
    if (const std::optional<DownloadResult> answer = downloader_->poll()) {
        page_answered(*answer);
    }
}

std::string ReleaseFeed::cached_note(const std::string& reason) const {
    if (cache_.releases.empty()) {
        return reason + ": no list to show yet";
    }
    return reason + ": showing the list from " + (cache_.fetched.empty() ? std::string("an earlier day") : cache_.fetched);
}

void ReleaseFeed::finish_with_cache(const std::string& reason) {
    loading_ = false;
    note_ = cached_note(reason);
    ++revision_;
}

void ReleaseFeed::finish(std::vector<SdkRelease> releases) {
    loading_ = false;
    note_.clear();
    cache_.releases = sort_releases(std::move(releases));
    cache_.etag = etag_;
    cache_.fetched = options_.today();
    static_cast<void>(save_release_cache(options_.cache_file, cache_, options_.platform));
    ++revision_;
}

void ReleaseFeed::page_answered(const DownloadResult& answer) {
    if (!answer.ok()) {
        finish_with_cache("offline");
        return;
    }
    const std::string headers = read_text_file(header_file_);
    const int status = answer.status != 0 ? answer.status : parse_http_status(headers);
    if (page_ == 0 && status == 304 && !cache_.releases.empty()) {
        // Nothing changed since the cache: it is as new as GitHub's list.
        cache_.fetched = options_.today();
        static_cast<void>(save_release_cache(options_.cache_file, cache_, options_.platform));
        loading_ = false;
        note_.clear();
        ++revision_;
        return;
    }
    if (status == 403 || status == 429) {
        finish_with_cache(options_.token.empty() ? "GitHub's rate limit is reached (set GITHUB_TOKEN to raise it)"
                                                 : "GitHub's rate limit is reached");
        return;
    }
    if (status != 200) {
        finish_with_cache("GitHub answered " + (status == 0 ? std::string("nothing") : std::to_string(status)));
        return;
    }

    const std::string body = read_text_file(body_file_);
    const std::expected<ReleasePage, std::string> page = parse_release_page(body, options_.platform);
    if (!page) {
        finish_with_cache("GitHub's answer could not be read");
        return;
    }
    if (page_ == 0) {
        etag_ = find_header(headers, "etag").value_or(std::string{});
    }
    seen_ += page->seen;
    collected_.insert(collected_.end(), page->releases.begin(), page->releases.end());
    ++page_;

    std::optional<std::string> next;
    if (const std::optional<std::string> link = find_header(headers, "link")) {
        next = next_page_url(*link);
    }
    if (next && next->starts_with(kApiHost) && seen_ < options_.max_releases && page->seen > 0) {
        request_page(*next);
        return;
    }
    finish(std::move(collected_));
}

}
