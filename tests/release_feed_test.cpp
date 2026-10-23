#include "release_feed.h"
#include "test_support.h"

#include <gtest/gtest.h>

using launcher::DownloadRequest;
using launcher::DownloadResult;
using launcher_test::FakeDownloader;
using launcher_test::TempDir;
using launcher_test::fixture;

namespace {

const launcher::Platform kLinux{.os = "linux", .arch = "x86_64"};

std::string link_to(std::string_view url) {
    return "link: <" + std::string(url) + ">; rel=\"next\"\r\n";
}

// A GitHub that answers the first page and, with a Link header, the second.
class FeedTest : public ::testing::Test {
protected:
    FeedTest()
        : downloader_([this](const DownloadRequest& request) { return answer(request); }) {}

    DownloadResult answer(const DownloadRequest& request) {
        ++calls_;
        if (!reply_error_.empty()) {
            return DownloadResult{.error = reply_error_};
        }
        const bool second = request.url.find("page=2") != std::string::npos;
        std::string headers = "HTTP/2 " + std::to_string(status_) + " \r\n";
        std::string body = second ? fixture("releases_page2.json") : first_body_;
        if (status_ == 200 && !second) {
            headers += "etag: W/\"abc\"\r\n";
            if (with_second_page_) {
                headers += link_to(next_url_);
            }
        }
        headers += "\r\n";
        dir_.write("work/releases-page.headers", headers);
        dir_.write("work/releases-page.json", status_ == 304 ? std::string{} : body);
        return DownloadResult{.status = status_};
    }

    launcher::ReleaseFeedOptions options() const {
        launcher::ReleaseFeedOptions options;
        options.platform = kLinux;
        options.cache_file = dir_.path() / "cache" / "available_sdks.txt";
        options.work_directory = dir_.path() / "work";
        options.today = [] { return std::string("2026-10-15"); };
        return options;
    }

    // Asks GitHub and waits for the answer.
    void refresh(launcher::ReleaseFeed& feed) {
        feed.refresh();
        for (int i = 0; i < 20 && feed.loading(); ++i) {
            feed.poll();
        }
        ASSERT_FALSE(feed.loading());
    }

    static std::vector<std::string> versions(const launcher::ReleaseFeed& feed) {
        std::vector<std::string> out;
        for (const auto& release : feed.releases()) {
            out.push_back(release.version);
        }
        return out;
    }

    TempDir dir_;
    FakeDownloader downloader_;
    int calls_ = 0;
    int status_ = 200;
    bool with_second_page_ = true;
    std::string reply_error_;
    std::string first_body_ = fixture("releases_page1.json");
    std::string next_url_ = "https://api.github.com/repositories/1/releases?per_page=100&page=2";
};

}

TEST_F(FeedTest, TheFirstFetchFollowsThePagesAndStoresTheCache) {
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    EXPECT_TRUE(feed.releases().empty());
    refresh(feed);

    EXPECT_EQ(versions(feed), (std::vector<std::string>{"0.2.0", "0.1.2a", "0.1.1a", "0.1.0a", "0.0.1a"}));
    EXPECT_EQ(feed.note(), "");
    ASSERT_EQ(downloader_.requests.size(), 2u);
    EXPECT_EQ(downloader_.requests[0].url, "https://api.github.com/repos/wismh/wind-engine/releases?per_page=100");
    EXPECT_EQ(downloader_.requests[1].url, next_url_);
    EXPECT_FALSE(downloader_.requests[0].download);
    EXPECT_TRUE(std::ranges::find(downloader_.requests[0].headers, "Accept: application/vnd.github+json") !=
                downloader_.requests[0].headers.end());
    for (const std::string& header : downloader_.requests[0].headers) {
        EXPECT_FALSE(header.starts_with("If-None-Match")) << "there is no cache yet";
    }

    const launcher::ReleaseCache stored = launcher::load_release_cache(options().cache_file, kLinux);
    EXPECT_EQ(stored.etag, "W/\"abc\"");
    EXPECT_EQ(stored.fetched, "2026-10-15");
    EXPECT_EQ(stored.releases.size(), 5u);
}

TEST_F(FeedTest, ASecondRunStartsFromTheCacheAndAsksWithTheEtag) {
    {
        launcher::ReleaseFeed first(downloader_, options());
        refresh(first);
    }
    status_ = 304;
    downloader_.requests.clear();
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    EXPECT_EQ(feed.releases().size(), 5u) << "the cached list shows before GitHub answers";
    refresh(feed);

    ASSERT_EQ(downloader_.requests.size(), 1u) << "304 ends it: no further pages";
    EXPECT_TRUE(std::ranges::find(downloader_.requests[0].headers, "If-None-Match: W/\"abc\"") !=
                downloader_.requests[0].headers.end());
    EXPECT_EQ(versions(feed), (std::vector<std::string>{"0.2.0", "0.1.2a", "0.1.1a", "0.1.0a", "0.0.1a"}));
    EXPECT_EQ(feed.note(), "") << "up to date is not worth a note";
}

TEST_F(FeedTest, ANewEtagReplacesTheCache) {
    {
        launcher::ReleaseFeed first(downloader_, options());
        refresh(first);
    }
    with_second_page_ = false;
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    refresh(feed);
    EXPECT_EQ(versions(feed), (std::vector<std::string>{"0.2.0", "0.1.2a", "0.1.1a"}));
    EXPECT_EQ(launcher::load_release_cache(options().cache_file, kLinux).releases.size(), 3u);
}

TEST_F(FeedTest, ANetworkErrorShowsTheCacheWithTheOfflineNote) {
    {
        launcher::ReleaseFeed first(downloader_, options());
        refresh(first);
    }
    reply_error_ = "(6) Could not resolve host: api.github.com";
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    refresh(feed);
    EXPECT_EQ(feed.releases().size(), 5u);
    EXPECT_EQ(feed.note(), "offline: showing the list from 2026-10-15");
}

TEST_F(FeedTest, OfflineWithoutACacheHasAnEmptyListAndSaysSo) {
    reply_error_ = "(6) Could not resolve host";
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    refresh(feed);
    EXPECT_TRUE(feed.releases().empty());
    EXPECT_NE(feed.note().find("offline"), std::string::npos) << feed.note();
    EXPECT_FALSE(std::filesystem::exists(options().cache_file)) << "nothing good to remember";
}

TEST_F(FeedTest, TheRateLimitShowsTheCacheWithANote) {
    {
        launcher::ReleaseFeed first(downloader_, options());
        refresh(first);
    }
    for (const int status : {403, 429}) {
        status_ = status;
        first_body_ = fixture("releases_rate_limit.json");
        launcher::ReleaseFeed feed(downloader_, options());
        feed.load_cache();
        refresh(feed);
        EXPECT_EQ(feed.releases().size(), 5u) << status;
        EXPECT_NE(feed.note().find("rate limit"), std::string::npos) << feed.note();
        EXPECT_NE(feed.note().find("2026-10-15"), std::string::npos) << feed.note();
        EXPECT_NE(feed.note().find("GITHUB_TOKEN"), std::string::npos) << "an unauthenticated client is told how to lift it";
    }
}

TEST_F(FeedTest, ATokenIsSentAndTheNoteDoesNotAskForOne) {
    status_ = 403;
    launcher::ReleaseFeedOptions with_token = options();
    with_token.token = "ghp_token";
    launcher::ReleaseFeed feed(downloader_, with_token);
    feed.load_cache();
    refresh(feed);
    ASSERT_EQ(downloader_.requests.size(), 1u);
    EXPECT_EQ(downloader_.requests[0].bearer_token, "ghp_token");
    EXPECT_EQ(feed.note().find("GITHUB_TOKEN"), std::string::npos) << feed.note();
}

TEST_F(FeedTest, AnAnswerThatIsNotAListKeepsTheCache) {
    {
        launcher::ReleaseFeed first(downloader_, options());
        refresh(first);
    }
    first_body_ = fixture("releases_rate_limit.json");  // a 200 whose body is an error object
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    refresh(feed);
    EXPECT_EQ(feed.releases().size(), 5u);
    EXPECT_NE(feed.note().find("could not be read"), std::string::npos) << feed.note();

    first_body_ = fixture("releases_invalid.json");
    refresh(feed);
    EXPECT_EQ(feed.releases().size(), 5u);
    EXPECT_NE(feed.note().find("could not be read"), std::string::npos);
}

TEST_F(FeedTest, AnotherStatusIsNamed) {
    status_ = 500;
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    refresh(feed);
    EXPECT_NE(feed.note().find("500"), std::string::npos) << feed.note();
}

TEST_F(FeedTest, ASecondPageThatFailsDoesNotMixTheListWithTheCache) {
    {
        launcher::ReleaseFeed first(downloader_, options());
        refresh(first);
    }
    launcher::ReleaseFeedOptions opts = options();
    int pages = 0;
    FakeDownloader flaky([&](const DownloadRequest& request) {
        if (++pages == 1) {
            return answer(request);
        }
        return DownloadResult{.error = "connection reset"};
    });
    launcher::ReleaseFeed feed(flaky, opts);
    feed.load_cache();
    refresh(feed);
    EXPECT_EQ(feed.releases().size(), 5u) << "the cached list, not the first page alone";
    EXPECT_NE(feed.note().find("offline"), std::string::npos);
}

TEST_F(FeedTest, PagesStopAtTheLimit) {
    launcher::ReleaseFeedOptions limited = options();
    limited.max_releases = 5;  // the first page holds 7
    launcher::ReleaseFeed feed(downloader_, limited);
    refresh(feed);
    EXPECT_EQ(downloader_.requests.size(), 1u);
    EXPECT_EQ(feed.releases().size(), 3u);
}

TEST_F(FeedTest, ALinkToAnotherHostIsNotFollowed) {
    next_url_ = "https://evil.example.com/releases?page=2";
    launcher::ReleaseFeed feed(downloader_, options());
    refresh(feed);
    EXPECT_EQ(downloader_.requests.size(), 1u);
}

TEST_F(FeedTest, OneRefreshAtATimeAndTheRevisionMoves) {
    downloader_.polls_before_answer = 3;
    launcher::ReleaseFeed feed(downloader_, options());
    feed.load_cache();
    const unsigned before = feed.revision();
    feed.refresh();
    EXPECT_TRUE(feed.loading());
    EXPECT_GT(feed.revision(), before);
    feed.refresh();
    EXPECT_EQ(downloader_.requests.size(), 1u) << "a refresh while one runs does nothing";
    feed.poll();
    EXPECT_TRUE(feed.loading()) << "the answer comes later";
    const unsigned during = feed.revision();
    downloader_.polls_before_answer = 0;
    for (int i = 0; i < 20 && feed.loading(); ++i) {
        feed.poll();
    }
    EXPECT_FALSE(feed.loading());
    EXPECT_GT(feed.revision(), during);
}

TEST(ReleaseCacheFile, RoundTripsAndRefusesWhatItDoesNotKnow) {
    launcher::ReleaseCache cache;
    cache.etag = "W/\"x\"";
    cache.fetched = "2026-10-15";
    launcher::SdkRelease release;
    release.version = "0.1.2a";
    release.parsed = *engine::parse_engine_version("0.1.2a");
    release.date = "2026-10-12";
    release.size = 123;
    release.archive_name = "wind-sdk-0.1.2a-linux-x86_64.tar.gz";
    release.archive_url = "https://github.com/a/" + release.archive_name;
    release.checksum_url = release.archive_url + ".sha256";
    cache.releases = {release};

    const std::string text = launcher::format_release_cache(cache, kLinux);
    const launcher::ReleaseCache again = launcher::parse_release_cache(text, kLinux);
    EXPECT_EQ(again.etag, cache.etag);
    EXPECT_EQ(again.fetched, cache.fetched);
    ASSERT_EQ(again.releases.size(), 1u);
    EXPECT_EQ(again.releases[0], release);

    EXPECT_TRUE(launcher::parse_release_cache(text, launcher::Platform{"windows", "x86_64"}).releases.empty())
            << "a cache of another platform";
    EXPECT_TRUE(launcher::parse_release_cache("", kLinux).releases.empty());
    EXPECT_TRUE(launcher::parse_release_cache("nonsense\n" + text, kLinux).releases.empty());
    std::string damaged = text;
    damaged.replace(damaged.find("0.1.2a\t"), 6, "0.1.x ");
    EXPECT_TRUE(launcher::parse_release_cache(damaged, kLinux).releases.empty());
    std::string http = text;
    http.replace(http.find("https://"), 8, "http://x");
    EXPECT_TRUE(launcher::parse_release_cache(http, kLinux).releases.empty()) << "only https links";
}

TEST(ReleaseCacheFile, RoundTripsTheSymbolsPackage) {
    launcher::ReleaseCache cache;
    cache.fetched = "2026-10-23";
    launcher::SdkRelease with;
    with.version = "0.2.9a";
    with.parsed = *engine::parse_engine_version("0.2.9a");
    with.size = 20886559;
    with.archive_name = "wind-sdk-0.2.9a-linux-x86_64.tar.gz";
    with.archive_url = "https://github.com/a/" + with.archive_name;
    with.checksum_url = with.archive_url + ".sha256";
    with.symbols = launcher::SdkPackage{.name = "wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz",
            .url = "https://github.com/a/wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz",
            .checksum_url = "https://github.com/a/wind-sdk-symbols-0.2.9a-linux-x86_64.tar.gz.sha256",
            .size = 85128922};
    launcher::SdkRelease without = with;
    without.version = "0.2.8a";
    without.parsed = *engine::parse_engine_version("0.2.8a");
    without.symbols.reset();
    cache.releases = {with, without};

    const std::string text = launcher::format_release_cache(cache, kLinux);
    const launcher::ReleaseCache again = launcher::parse_release_cache(text, kLinux);
    ASSERT_EQ(again.releases.size(), 2u);
    EXPECT_EQ(again.releases[0], with);
    EXPECT_EQ(again.releases[1], without);
    EXPECT_FALSE(again.releases[1].symbols.has_value());

    std::string http = text;
    http.replace(http.find("https://github.com/a/wind-sdk-symbols"), 8, "http://x");
    EXPECT_TRUE(launcher::parse_release_cache(http, kLinux).releases.empty()) << "only https links";
}

TEST(ReleaseCacheFile, ACacheOfTheFirstFormatIsDropped) {
    // It has no symbols; keeping it would let a 304 answer hide them for ever.
    const std::string old_cache =
            "wind-launcher-sdk-cache 1\netag\tW/\"x\"\nfetched\t2026-10-15\nplatform\tlinux\tx86_64\n"
            "release\t0.1.2a\t2026-10-12\t123\twind-sdk-0.1.2a-linux-x86_64.tar.gz\thttps://a/b\thttps://a/b.sha256\n";
    EXPECT_TRUE(launcher::parse_release_cache(old_cache, kLinux).releases.empty());
    EXPECT_TRUE(launcher::parse_release_cache(old_cache, kLinux).etag.empty());
}

TEST(ReleaseCacheFile, ATodayInUtc) {
    const std::string today = launcher::today_utc();
    ASSERT_EQ(today.size(), 10u);
    EXPECT_EQ(today[4], '-');
    EXPECT_EQ(today[7], '-');
}
