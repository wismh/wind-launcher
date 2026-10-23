#pragma once

#include "sdk_catalog.h"

#include <engine/project/engine_version.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// The operating system and processor of an SDK archive, with the names of the release assets: windows|linux|macos and
// x86_64|arm64 (tools/ci/setup_sdk.py of the engine).
struct Platform {
    std::string os;
    std::string arch;

    friend bool operator==(const Platform&, const Platform&) = default;
};

// The platform this launcher was built for.
[[nodiscard]] Platform current_platform();

// `wind-sdk-<version>-<os>-<arch>.zip` (Windows) or `.tar.gz`, and the same with `.sha256`.
[[nodiscard]] std::string sdk_archive_name(std::string_view version, const Platform& platform);
[[nodiscard]] std::string sdk_checksum_name(std::string_view version, const Platform& platform);
// `wind-sdk-<version>-<os>-<arch>`, the one directory an archive holds.
[[nodiscard]] std::string sdk_top_directory(std::string_view version, const Platform& platform);

// The version in an asset name that is the SDK archive of `platform`; nullopt for any other name, another platform, an
// unparsable version, or a `-dev` one (nobody publishes those).
[[nodiscard]] std::optional<std::string> sdk_asset_version(std::string_view asset_name, const Platform& platform);

// `wind-sdk-symbols-<version>-<os>-<arch>.zip` (Windows) or `.tar.gz`: the debug information split off the SDK, unpacked
// over it. Its checksum file adds `.sha256`.
[[nodiscard]] std::string symbols_archive_name(std::string_view version, const Platform& platform);
// `wind-sdk-symbols-<version>-<os>-<arch>`, the one directory that archive holds.
[[nodiscard]] std::string symbols_top_directory(std::string_view version, const Platform& platform);

// One archive of a release with its checksum file: the SDK itself, or an optional package over it.
struct SdkPackage {
    std::string name;
    std::string url;
    std::string checksum_url;
    // The archive's size in bytes, from the API.
    std::uint64_t size = 0;

    friend bool operator==(const SdkPackage&, const SdkPackage&) = default;
};

// One SDK a GitHub release offers for this platform: its archive and the checksum file.
struct SdkRelease {
    std::string version;
    engine::EngineVersion parsed;
    // The release's date, `2026-10-11` (the day of its `published_at`).
    std::string date;
    std::string archive_name;
    std::string archive_url;
    std::string checksum_url;
    // The archive's size in bytes, from the API.
    std::uint64_t size = 0;
    // The debug symbols of this SDK, when the release has them for this platform. Releases from before the split have
    // none.
    std::optional<SdkPackage> symbols;

    friend bool operator==(const SdkRelease&, const SdkRelease&) = default;
};

struct ReleasePage {
    std::vector<SdkRelease> releases;
    // How many releases the page held, usable or not: the 500 limit counts those.
    std::size_t seen = 0;
};

// The SDKs of one page of `GET /repos/<repo>/releases`. Drafts, releases without the archive of `platform` or without
// its `.sha256`, archives whose version does not parse, and download links that are not https are left out. The symbols
// package is kept only with its `.sha256` and an https link; without them the SDK is listed with no symbols. A document
// that is not a JSON array is an error.
[[nodiscard]] std::expected<ReleasePage, std::string> parse_release_page(std::string_view json,
        const Platform& platform);

// The `rel="next"` URL of a `Link` response header value, or nullopt.
[[nodiscard]] std::optional<std::string> next_page_url(std::string_view link_header);

// Newest first (engine::EngineVersion order), one entry per version (the first one a page offered wins).
[[nodiscard]] std::vector<SdkRelease> sort_releases(std::vector<SdkRelease> releases);

// One row of the Available list.
struct AvailableSdk {
    SdkRelease release;
    bool installed = false;
    // The newest version on the list.
    bool newest = false;
};

// `releases` as the Available list: sorted, marked installed when an SDK of exactly that version is in `installed`, and
// the first one marked newest.
[[nodiscard]] std::vector<AvailableSdk> build_available(
        std::vector<SdkRelease> releases, const std::vector<SdkEntry>& installed);

// "alpha", "beta", or "release".
[[nodiscard]] std::string_view stage_name(const engine::EngineVersion& version);

// "136.9 MB", "812 KB", "12 B".
[[nodiscard]] std::string format_size(std::uint64_t bytes);

}
