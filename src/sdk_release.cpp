#include "sdk_release.h"

#include "json.h"

#include <algorithm>
#include <cstdio>

namespace launcher {
namespace {

constexpr std::string_view kPrefix = "wind-sdk-";
constexpr std::string_view kSymbolsPrefix = "wind-sdk-symbols-";

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

const std::string* string_member(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object.find(key);
    return value != nullptr && value->is(JsonValue::Kind::String) ? &value->string : nullptr;
}

const JsonValue* find_asset(const JsonValue& assets, std::string_view name) {
    for (const JsonValue& asset : assets.array) {
        const std::string* asset_name = asset.is(JsonValue::Kind::Object) ? string_member(asset, "name") : nullptr;
        if (asset_name != nullptr && *asset_name == name) {
            return &asset;
        }
    }
    return nullptr;
}

const std::string* https_url(const JsonValue* asset) {
    const std::string* url = asset != nullptr ? string_member(*asset, "browser_download_url") : nullptr;
    return url != nullptr && url->starts_with("https://") ? url : nullptr;
}

std::uint64_t size_member(const JsonValue& asset) {
    const JsonValue* size = asset.find("size");
    return size != nullptr && size->is(JsonValue::Kind::Number) && size->number >= 0
                   ? static_cast<std::uint64_t>(size->number)
                   : 0;
}

// The archive `name` of the release with its `.sha256`, or nullopt when either is missing or not an https link.
std::optional<SdkPackage> find_package(const JsonValue& assets, const std::string& name) {
    const JsonValue* archive = find_asset(assets, name);
    const std::string* url = https_url(archive);
    const std::string* checksum_url = https_url(find_asset(assets, name + ".sha256"));
    if (url == nullptr || checksum_url == nullptr) {
        return std::nullopt;
    }
    return SdkPackage{.name = name, .url = *url, .checksum_url = *checksum_url, .size = size_member(*archive)};
}

bool is_day(std::string_view text) {
    if (text.size() < 10) {
        return false;
    }
    for (std::size_t i = 0; i < 10; ++i) {
        const bool dash = i == 4 || i == 7;
        if (dash ? text[i] != '-' : (text[i] < '0' || text[i] > '9')) {
            return false;
        }
    }
    return true;
}

}

Platform current_platform() {
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "macos";
#else
    const char* os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* arch = "arm64";
#else
    const char* arch = "x86_64";
#endif
    return Platform{.os = os, .arch = arch};
}

std::string sdk_top_directory(std::string_view version, const Platform& platform) {
    return std::string(kPrefix) + std::string(version) + "-" + platform.os + "-" + platform.arch;
}

std::string sdk_archive_name(std::string_view version, const Platform& platform) {
    return sdk_top_directory(version, platform) + (platform.os == "windows" ? ".zip" : ".tar.gz");
}

std::string symbols_top_directory(std::string_view version, const Platform& platform) {
    return std::string(kSymbolsPrefix) + std::string(version) + "-" + platform.os + "-" + platform.arch;
}

std::string symbols_archive_name(std::string_view version, const Platform& platform) {
    return symbols_top_directory(version, platform) + (platform.os == "windows" ? ".zip" : ".tar.gz");
}

std::string sdk_checksum_name(std::string_view version, const Platform& platform) {
    return sdk_archive_name(version, platform) + ".sha256";
}

std::optional<std::string> sdk_asset_version(std::string_view asset_name, const Platform& platform) {
    const std::string extension = platform.os == "windows" ? ".zip" : ".tar.gz";
    const std::string tail = "-" + platform.os + "-" + platform.arch + extension;
    if (asset_name.size() <= kPrefix.size() + tail.size() || asset_name.substr(0, kPrefix.size()) != kPrefix ||
            !ends_with(asset_name, tail)) {
        return std::nullopt;
    }
    const std::string version(asset_name.substr(kPrefix.size(), asset_name.size() - kPrefix.size() - tail.size()));
    const std::optional<engine::EngineVersion> parsed = engine::parse_engine_version(version);
    if (!parsed || parsed->dev) {
        return std::nullopt;
    }
    return version;
}

std::expected<ReleasePage, std::string> parse_release_page(std::string_view json, const Platform& platform) {
    const std::expected<JsonValue, std::string> document = parse_json(json);
    if (!document) {
        return std::unexpected(document.error());
    }
    if (!document->is(JsonValue::Kind::Array)) {
        const std::string* message = string_member(*document, "message");
        return std::unexpected("GitHub did not answer with a list of releases" +
                               (message != nullptr ? ": " + *message : std::string{}));
    }

    ReleasePage page;
    for (const JsonValue& release : document->array) {
        ++page.seen;
        if (!release.is(JsonValue::Kind::Object)) {
            continue;
        }
        const JsonValue* draft = release.find("draft");
        if (draft != nullptr && draft->is(JsonValue::Kind::Bool) && draft->boolean) {
            continue;
        }
        const JsonValue* assets = release.find("assets");
        if (assets == nullptr || !assets->is(JsonValue::Kind::Array)) {
            continue;
        }
        for (const JsonValue& asset : assets->array) {
            const std::string* name = asset.is(JsonValue::Kind::Object) ? string_member(asset, "name") : nullptr;
            if (name == nullptr) {
                continue;
            }
            const std::optional<std::string> version = sdk_asset_version(*name, platform);
            if (!version) {
                continue;
            }
            // The checksum is an asset of the same release, named after the archive.
            const std::optional<SdkPackage> package = find_package(*assets, *name);
            if (!package) {
                continue;
            }
            SdkRelease sdk;
            sdk.version = *version;
            sdk.parsed = *engine::parse_engine_version(*version);
            const std::string* published = string_member(release, "published_at");
            if (published == nullptr) {
                published = string_member(release, "created_at");
            }
            if (published != nullptr && is_day(*published)) {
                sdk.date = published->substr(0, 10);
            }
            sdk.archive_name = package->name;
            sdk.archive_url = package->url;
            sdk.checksum_url = package->checksum_url;
            sdk.size = package->size;
            sdk.symbols = find_package(*assets, symbols_archive_name(*version, platform));
            page.releases.push_back(std::move(sdk));
            break;
        }
    }
    return page;
}

std::optional<std::string> next_page_url(std::string_view link_header) {
    // <https://api.github.com/...&page=2>; rel="next", <...>; rel="last"
    std::size_t pos = 0;
    while (pos < link_header.size()) {
        const std::size_t open = link_header.find('<', pos);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = link_header.find('>', open);
        if (close == std::string_view::npos) {
            break;
        }
        const std::size_t next_entry = link_header.find(',', close);
        const std::string_view params =
                link_header.substr(close + 1, next_entry == std::string_view::npos ? std::string_view::npos
                                                                                    : next_entry - close - 1);
        if (params.find("rel=\"next\"") != std::string_view::npos || params.find("rel=next") != std::string_view::npos) {
            return std::string(link_header.substr(open + 1, close - open - 1));
        }
        if (next_entry == std::string_view::npos) {
            break;
        }
        pos = next_entry + 1;
    }
    return std::nullopt;
}

std::vector<SdkRelease> sort_releases(std::vector<SdkRelease> releases) {
    std::ranges::stable_sort(releases, [](const SdkRelease& a, const SdkRelease& b) { return a.parsed > b.parsed; });
    releases.erase(std::ranges::unique(releases, [](const SdkRelease& a, const SdkRelease& b) {
        return a.version == b.version;
    }).begin(), releases.end());
    return releases;
}

std::vector<AvailableSdk> build_available(std::vector<SdkRelease> releases, const std::vector<SdkEntry>& installed) {
    std::vector<AvailableSdk> rows;
    for (SdkRelease& release : sort_releases(std::move(releases))) {
        AvailableSdk row;
        row.installed = std::ranges::any_of(
                installed, [&](const SdkEntry& sdk) { return sdk.manifest.version == release.version; });
        row.release = std::move(release);
        rows.push_back(std::move(row));
    }
    if (!rows.empty()) {
        rows.front().newest = true;
    }
    return rows;
}

std::string_view stage_name(const engine::EngineVersion& version) {
    switch (version.stage) {
        case 'a':
            return "alpha";
        case 'b':
            return "beta";
        default:
            return "release";
    }
}

std::string format_size(std::uint64_t bytes) {
    char text[32];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(text, sizeof text, "%.0f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    }
    return text;
}

}
