#include "sdk_release.h"
#include "test_support.h"

#include <gtest/gtest.h>

using launcher::Platform;
using launcher_test::fixture;

namespace {

const Platform kLinux{.os = "linux", .arch = "x86_64"};
const Platform kWindows{.os = "windows", .arch = "x86_64"};
const Platform kMac{.os = "macos", .arch = "arm64"};

std::vector<std::string> versions(const std::vector<launcher::SdkRelease>& releases) {
    std::vector<std::string> out;
    for (const auto& release : releases) {
        out.push_back(release.version);
    }
    return out;
}

launcher::SdkEntry installed(std::string_view version) {
    launcher::SdkEntry entry;
    entry.manifest.version = std::string(version);
    return entry;
}

}

TEST(SdkAssets, NamesFollowTheReleaseWorkflow) {
    EXPECT_EQ(launcher::sdk_archive_name("0.1.7a", kWindows), "wind-sdk-0.1.7a-windows-x86_64.zip");
    EXPECT_EQ(launcher::sdk_archive_name("0.1.7a", kLinux), "wind-sdk-0.1.7a-linux-x86_64.tar.gz");
    EXPECT_EQ(launcher::sdk_archive_name("0.1.7a", kMac), "wind-sdk-0.1.7a-macos-arm64.tar.gz");
    EXPECT_EQ(launcher::sdk_checksum_name("0.1.7a", kLinux), "wind-sdk-0.1.7a-linux-x86_64.tar.gz.sha256");
    EXPECT_EQ(launcher::sdk_top_directory("0.1.7a", kMac), "wind-sdk-0.1.7a-macos-arm64");
}

TEST(SdkAssets, TheVersionOfAnArchiveOfThisPlatform) {
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-0.1.7a-linux-x86_64.tar.gz", kLinux), "0.1.7a");
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-12.0.100-windows-x86_64.zip", kWindows), "12.0.100");
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-0.2.0b-macos-arm64.tar.gz", kMac), "0.2.0b");
}

TEST(SdkAssets, NamesThatAreNotThisPlatformsArchive) {
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-0.1.7a-windows-x86_64.zip", kLinux), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-0.1.7a-linux-x86_64.tar.gz", kWindows), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-0.1.7a-linux-arm64.tar.gz", kLinux), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-0.1.7a-linux-x86_64.tar.gz.sha256", kLinux), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-0.1.7a-linux-x86_64.zip", kLinux), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("source.tar.gz", kLinux), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("", kLinux), std::nullopt);
}

TEST(SdkAssets, MalformedVersionsAreIgnored) {
    for (const char* version : {"0.1.x", "0.1", "0.1.7c", "0.1.7a-dev", "-1.0.0", "0.1.7a.1", "v0.1.7a", ""}) {
        EXPECT_EQ(launcher::sdk_asset_version(std::string("wind-sdk-") + version + "-linux-x86_64.tar.gz", kLinux),
                std::nullopt)
                << version;
    }
}

TEST(SdkAssets, TheSymbolsPackageIsNamedAfterTheSdk) {
    EXPECT_EQ(launcher::symbols_archive_name("0.2.9a", kWindows), "wind-sdk-symbols-0.2.9a-windows-x86_64.zip");
    EXPECT_EQ(launcher::symbols_archive_name("0.2.9a", kLinux), "wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz");
    EXPECT_EQ(launcher::symbols_archive_name("0.2.9a", kMac), "wind-sdk-symbols-0.2.9a-macos-arm64.tar.gz");
    EXPECT_EQ(launcher::symbols_top_directory("0.2.9a", kMac), "wind-sdk-symbols-0.2.9a-macos-arm64");
}

TEST(SdkAssets, OtherPackagesOfTheReleaseAreNotSdks) {
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz", kLinux), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-symbols-0.2.9a-windows-x86_64.zip", kWindows), std::nullopt);
    EXPECT_EQ(launcher::sdk_asset_version("wind-sdk-source-0.2.9a.tar.gz", kLinux), std::nullopt);
}

TEST(ReleasePage, TheSymbolsPackageComesWithTheSdk) {
    const std::string json = R"([{"published_at": "2026-10-23T10:04:51Z", "assets": [
        {"name": "wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz", "size": 85128922, "browser_download_url": "https://example.com/s.tar.gz"},
        {"name": "wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz.sha256", "size": 110, "browser_download_url": "https://example.com/s.tar.gz.sha256"},
        {"name": "wind-sdk-0.2.9a-linux-x86_64.tar.gz", "size": 20886559, "browser_download_url": "https://example.com/a.tar.gz"},
        {"name": "wind-sdk-0.2.9a-linux-x86_64.tar.gz.sha256", "size": 102, "browser_download_url": "https://example.com/a.tar.gz.sha256"},
        {"name": "wind-sdk-symbols-0.2.9a-windows-x86_64.zip", "size": 1, "browser_download_url": "https://example.com/w.zip"},
        {"name": "wind-sdk-source-0.2.9a.tar.gz", "size": 61412887, "browser_download_url": "https://example.com/src.tar.gz"}]}])";
    const auto page = launcher::parse_release_page(json, kLinux);
    ASSERT_TRUE(page.has_value()) << page.error();
    ASSERT_EQ(page->releases.size(), 1u) << "the symbols and the source are not SDKs of their own";
    const launcher::SdkRelease& release = page->releases[0];
    EXPECT_EQ(release.version, "0.2.9a");
    EXPECT_EQ(release.archive_name, "wind-sdk-0.2.9a-linux-x86_64.tar.gz");
    EXPECT_EQ(release.size, 20886559u);
    ASSERT_TRUE(release.symbols.has_value());
    EXPECT_EQ(release.symbols->name, "wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz");
    EXPECT_EQ(release.symbols->url, "https://example.com/s.tar.gz");
    EXPECT_EQ(release.symbols->checksum_url, "https://example.com/s.tar.gz.sha256");
    EXPECT_EQ(release.symbols->size, 85128922u);

    const auto windows = launcher::parse_release_page(json, kWindows);
    ASSERT_TRUE(windows.has_value());
    EXPECT_TRUE(windows->releases.empty()) << "symbols alone are no SDK";
}

TEST(ReleasePage, ASdkWithoutUsableSymbolsIsListedWithoutThem) {
    const auto sdk = [](const std::string& symbols_assets) {
        return R"([{"published_at": "2026-10-12T08:30:00Z", "assets": [
        {"name": "wind-sdk-0.1.2a-linux-x86_64.tar.gz", "size": 5, "browser_download_url": "https://example.com/a"},
        {"name": "wind-sdk-0.1.2a-linux-x86_64.tar.gz.sha256", "size": 5, "browser_download_url": "https://example.com/a.sha256"})" +
               symbols_assets + "]}]";
    };
    // Before the split: no symbols package at all.
    auto page = launcher::parse_release_page(sdk(""), kLinux);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->releases.size(), 1u);
    EXPECT_FALSE(page->releases[0].symbols.has_value());

    // No checksum for the symbols.
    page = launcher::parse_release_page(sdk(R"(,
        {"name": "wind-sdk-symbols-0.1.2a-linux-x86_64.tar.gz", "size": 5, "browser_download_url": "https://example.com/s"})"),
            kLinux);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->releases.size(), 1u);
    EXPECT_FALSE(page->releases[0].symbols.has_value());

    // A link that is not https.
    page = launcher::parse_release_page(sdk(R"(,
        {"name": "wind-sdk-symbols-0.1.2a-linux-x86_64.tar.gz", "size": 5, "browser_download_url": "http://example.com/s"},
        {"name": "wind-sdk-symbols-0.1.2a-linux-x86_64.tar.gz.sha256", "size": 5, "browser_download_url": "https://example.com/s.sha256"})"),
            kLinux);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->releases.size(), 1u);
    EXPECT_FALSE(page->releases[0].symbols.has_value());
}

TEST(ReleasePage, TheLinuxSdksOfTheFirstPage) {
    const auto page = launcher::parse_release_page(fixture("releases_page1.json"), kLinux);
    ASSERT_TRUE(page.has_value()) << page.error();
    EXPECT_EQ(page->seen, 7u);
    // The draft, the release without a Linux archive, the malformed name, and the archive without a checksum are out.
    EXPECT_EQ(versions(page->releases), (std::vector<std::string>{"0.1.2a", "0.2.0", "0.1.1a"}));
    const launcher::SdkRelease& first = page->releases[0];
    EXPECT_EQ(first.archive_name, "wind-sdk-0.1.2a-linux-x86_64.tar.gz");
    EXPECT_EQ(first.archive_url,
            "https://github.com/wismh/wind-engine/releases/download/v0.1.2a/wind-sdk-0.1.2a-linux-x86_64.tar.gz");
    EXPECT_EQ(first.checksum_url, first.archive_url + ".sha256");
    EXPECT_EQ(first.size, 136883658u);
    EXPECT_EQ(first.date, "2026-10-12");
    EXPECT_EQ(first.parsed.stage, 'a');
}

TEST(ReleasePage, EachPlatformGetsItsOwnArchive) {
    const auto windows = launcher::parse_release_page(fixture("releases_page1.json"), kWindows);
    ASSERT_TRUE(windows.has_value());
    EXPECT_EQ(versions(windows->releases), (std::vector<std::string>{"0.1.2a", "0.1.1a", "0.1.0b"}));
    EXPECT_EQ(windows->releases[0].archive_name, "wind-sdk-0.1.2a-windows-x86_64.zip");
    const auto mac = launcher::parse_release_page(fixture("releases_page1.json"), kMac);
    ASSERT_TRUE(mac.has_value());
    EXPECT_EQ(versions(mac->releases), (std::vector<std::string>{"0.1.2a", "0.1.1a"}));
}

TEST(ReleasePage, EmptyAndBrokenDocuments) {
    const auto empty = launcher::parse_release_page(fixture("releases_empty.json"), kLinux);
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->releases.empty());
    EXPECT_EQ(empty->seen, 0u);

    EXPECT_FALSE(launcher::parse_release_page(fixture("releases_invalid.json"), kLinux).has_value());
    EXPECT_FALSE(launcher::parse_release_page("", kLinux).has_value());
    const auto limited = launcher::parse_release_page(fixture("releases_rate_limit.json"), kLinux);
    ASSERT_FALSE(limited.has_value());
    EXPECT_NE(limited.error().find("rate limit"), std::string::npos) << limited.error();
}

TEST(ReleasePage, OddItemsAreSkippedNotFatal) {
    const auto page = launcher::parse_release_page(
            R"([1, "x", null, {}, {"assets": 5}, {"assets": [3, {"name": 4}, {"name": "wind-sdk-0.1.0a-linux-x86_64.tar.gz"}]}])",
            kLinux);
    ASSERT_TRUE(page.has_value()) << page.error();
    EXPECT_TRUE(page->releases.empty());
    EXPECT_EQ(page->seen, 6u);
}

TEST(ReleasePage, ADownloadLinkThatIsNotHttpsIsRefused) {
    const std::string json = R"([{"draft": false, "published_at": "2026-10-12T08:30:00Z", "assets": [
        {"name": "wind-sdk-0.1.2a-linux-x86_64.tar.gz", "size": 5, "browser_download_url": "http://example.com/a"},
        {"name": "wind-sdk-0.1.2a-linux-x86_64.tar.gz.sha256", "size": 5, "browser_download_url": "https://example.com/a.sha256"}]}])";
    const auto page = launcher::parse_release_page(json, kLinux);
    ASSERT_TRUE(page.has_value());
    EXPECT_TRUE(page->releases.empty());
}

TEST(ReleasePage, ADateThatIsNotADayIsLeftEmpty) {
    const std::string json = R"([{"published_at": "yesterday", "assets": [
        {"name": "wind-sdk-0.1.2a-linux-x86_64.tar.gz", "size": 5, "browser_download_url": "https://example.com/a"},
        {"name": "wind-sdk-0.1.2a-linux-x86_64.tar.gz.sha256", "size": 5, "browser_download_url": "https://example.com/a.sha256"}]}])";
    const auto page = launcher::parse_release_page(json, kLinux);
    ASSERT_TRUE(page.has_value());
    ASSERT_EQ(page->releases.size(), 1u);
    EXPECT_EQ(page->releases[0].date, "");
}

TEST(ReleasePage, ThePaginationLinkHeader) {
    EXPECT_EQ(launcher::next_page_url(
                      "<https://api.github.com/repositories/1/releases?per_page=100&page=2>; rel=\"next\", "
                      "<https://api.github.com/repositories/1/releases?per_page=100&page=5>; rel=\"last\""),
            "https://api.github.com/repositories/1/releases?per_page=100&page=2");
    EXPECT_EQ(launcher::next_page_url(
                      "<https://api.github.com/r?page=1>; rel=\"prev\", <https://api.github.com/r?page=3>; rel=\"next\""),
            "https://api.github.com/r?page=3");
    // The last page has no next.
    EXPECT_EQ(launcher::next_page_url(
                      "<https://api.github.com/r?page=1>; rel=\"prev\", <https://api.github.com/r?page=1>; rel=\"first\""),
            std::nullopt);
    EXPECT_EQ(launcher::next_page_url(""), std::nullopt);
    EXPECT_EQ(launcher::next_page_url("garbage"), std::nullopt);
    EXPECT_EQ(launcher::next_page_url("<https://x"), std::nullopt);
}

TEST(AvailableSdks, NewestFirstByTheEngineOrderNotByText) {
    std::vector<launcher::SdkRelease> releases;
    for (const char* version : {"0.1.99a", "0.1.100a", "0.2.0b", "0.2.0", "0.2.0a", "0.1.2a"}) {
        launcher::SdkRelease release;
        release.version = version;
        release.parsed = *engine::parse_engine_version(version);
        releases.push_back(release);
    }
    const auto rows = launcher::build_available(std::move(releases), {});
    std::vector<std::string> order;
    for (const auto& row : rows) {
        order.push_back(row.release.version);
    }
    EXPECT_EQ(order, (std::vector<std::string>{"0.2.0", "0.2.0b", "0.2.0a", "0.1.100a", "0.1.99a", "0.1.2a"}));
}

TEST(AvailableSdks, InstalledAndNewestAreMarked) {
    const auto page = launcher::parse_release_page(fixture("releases_page1.json"), kLinux);
    ASSERT_TRUE(page.has_value());
    const auto rows = launcher::build_available(page->releases, {installed("0.1.1a"), installed("0.0.5a")});
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].release.version, "0.2.0");
    EXPECT_TRUE(rows[0].newest);
    EXPECT_FALSE(rows[0].installed);
    EXPECT_FALSE(rows[1].newest);
    EXPECT_FALSE(rows[1].installed);
    EXPECT_EQ(rows[2].release.version, "0.1.1a");
    EXPECT_FALSE(rows[2].newest);
    EXPECT_TRUE(rows[2].installed);
}

TEST(AvailableSdks, AVersionIsInstalledOnlyByExactText) {
    const auto page = launcher::parse_release_page(fixture("releases_page1.json"), kLinux);
    ASSERT_TRUE(page.has_value());
    const auto rows = launcher::build_available(page->releases, {installed("0.1.1"), installed("0.1.1b")});
    for (const auto& row : rows) {
        EXPECT_FALSE(row.installed) << row.release.version;
    }
}

TEST(AvailableSdks, OneRowPerVersionAndEmptyIsEmpty) {
    launcher::SdkRelease release;
    release.version = "0.1.2a";
    release.parsed = *engine::parse_engine_version("0.1.2a");
    release.date = "first";
    launcher::SdkRelease again = release;
    again.date = "second";
    const auto rows = launcher::build_available({release, again}, {});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].release.date, "first");
    EXPECT_TRUE(launcher::build_available({}, {}).empty());
}

TEST(AvailableSdks, StageAndSizeTexts) {
    EXPECT_EQ(launcher::stage_name(*engine::parse_engine_version("0.1.0a")), "alpha");
    EXPECT_EQ(launcher::stage_name(*engine::parse_engine_version("0.1.0b")), "beta");
    EXPECT_EQ(launcher::stage_name(*engine::parse_engine_version("0.1.0")), "release");
    EXPECT_EQ(launcher::format_size(0), "0 B");
    EXPECT_EQ(launcher::format_size(1023), "1023 B");
    EXPECT_EQ(launcher::format_size(2048), "2 KB");
    EXPECT_EQ(launcher::format_size(136883658), "130.5 MB");
    EXPECT_EQ(launcher::format_size(3ull << 30), "3.0 GB");
}

TEST(CurrentPlatform, UsesTheNamesOfTheAssets) {
    const Platform platform = launcher::current_platform();
    EXPECT_TRUE(platform.os == "windows" || platform.os == "linux" || platform.os == "macos") << platform.os;
    EXPECT_TRUE(platform.arch == "x86_64" || platform.arch == "arm64") << platform.arch;
}
