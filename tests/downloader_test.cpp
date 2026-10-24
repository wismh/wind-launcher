#include "downloader.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>

using launcher_test::TempDir;

namespace {

// An IHttpClient that does not touch the network: it records the request, feeds its sink the body of the prepared
// answer the way the engine does for a 2xx, and answers at once.
class FakeHttp final : public engine::IHttpClient {
public:
    bool init() override {
        return true;
    }
    void dispose() override {}
    [[nodiscard]] bool is_supported() const override {
        return true;
    }
    [[nodiscard]] engine::HttpCall send(engine::HttpRequest request) override {
        requests.push_back(request);
        engine::HttpResult next = result;
        if (!results.empty()) {
            next = results.front();
            results.erase(results.begin());
        }
        if (next && next->status >= 200 && next->status < 300 && request.sink) {
            const std::span<const std::byte> bytes(
                    reinterpret_cast<const std::byte*>(next->body.data()), next->body.size());
            if (!request.sink(bytes)) {
                next = std::unexpected(engine::HttpError::SinkAborted);
            } else {
                next->body.clear();
            }
        }
        return engine::HttpCall::resolved(std::move(next));
    }

    std::vector<engine::HttpRequest> requests;
    engine::HttpResult result = engine::HttpResponse{.status = 200};
    // Used one by one, then `result`.
    std::vector<engine::HttpResult> results;
};

engine::HttpResponse response(int status, std::string body = {}, std::vector<engine::HttpHeader> headers = {}) {
    return engine::HttpResponse{.status = status, .headers = std::move(headers), .body = std::move(body)};
}

std::optional<launcher::DownloadResult> drain(launcher::HttpDownloader& downloader) {
    for (int i = 0; i < 10; ++i) {
        if (auto answer = downloader.poll()) {
            return answer;
        }
    }
    return std::nullopt;
}

}

TEST(HttpHeaders, TheStatusOfTheLastResponse) {
    EXPECT_EQ(launcher::parse_http_status("HTTP/2 200 \r\netag: \"a\"\r\n\r\n"), 200);
    EXPECT_EQ(launcher::parse_http_status("HTTP/1.1 304 Not Modified\r\n\r\n"), 304);
    EXPECT_EQ(launcher::parse_http_status("HTTP/1.1 403 Forbidden\nx-ratelimit-remaining: 0\n\n"), 403);
    EXPECT_EQ(launcher::parse_http_status(
                      "HTTP/1.1 302 Found\r\nlocation: https://x\r\n\r\nHTTP/2 200 \r\ncontent-length: 5\r\n\r\n"),
            200);
    EXPECT_EQ(launcher::parse_http_status(""), 0);
    EXPECT_EQ(launcher::parse_http_status("garbage"), 0);
}

TEST(HttpHeaders, AHeaderOfTheLastResponseIgnoringCase) {
    const std::string dump =
            "HTTP/1.1 302 Found\r\nETag: \"redirect\"\r\n\r\n"
            "HTTP/2 200 \r\nETag: W/\"abc\"\r\nLink:   <https://api.github.com/x?page=2>; rel=\"next\"\r\n\r\n";
    EXPECT_EQ(launcher::find_header(dump, "etag"), "W/\"abc\"");
    EXPECT_EQ(launcher::find_header(dump, "LINK"), "<https://api.github.com/x?page=2>; rel=\"next\"");
    EXPECT_EQ(launcher::find_header(dump, "x-missing"), std::nullopt);
    EXPECT_EQ(launcher::find_header("", "etag"), std::nullopt);
}

TEST(HeaderLines, AreSplitAtTheFirstColonAndTrimmed) {
    const auto headers = launcher::parse_header_lines({"Accept: application/json", "If-None-Match:W/\"a:b\"", "junk"});
    ASSERT_EQ(headers.size(), 2u);
    EXPECT_EQ(headers[0].name, "Accept");
    EXPECT_EQ(headers[0].value, "application/json");
    EXPECT_EQ(headers[1].name, "If-None-Match");
    EXPECT_EQ(headers[1].value, "W/\"a:b\"");
}

TEST(HeaderDump, IsWhatTheStatusAndHeaderReadersParse) {
    const std::string dump = launcher::format_header_dump(
            response(200, {}, {{"ETag", "W/\"abc\""}, {"Link", "<x>; rel=\"next\""}}));
    EXPECT_EQ(launcher::parse_http_status(dump), 200);
    EXPECT_EQ(launcher::find_header(dump, "etag"), "W/\"abc\"");
    EXPECT_EQ(launcher::find_header(dump, "link"), "<x>; rel=\"next\"");
}

TEST(HttpDownloader, AnApiCallDoesNotFollowRedirects) {
    const TempDir dir;
    FakeHttp http;
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{
            .url = "https://api.github.com/x", .output = dir.path() / "b.json", .download = false});
    ASSERT_EQ(http.requests.size(), 1u);
    EXPECT_EQ(http.requests[0].max_redirects, 0);
    EXPECT_EQ(http.requests[0].min_bytes_per_second, 0u);
}

TEST(HttpDownloader, ADownloadFollowsRedirectsOnlyToHttpsAndGivesUpOnAStall) {
    const TempDir dir;
    FakeHttp http;
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://github.com/a/b", .output = dir.path() / "o.bin"});
    ASSERT_EQ(http.requests.size(), 1u);
    EXPECT_GT(http.requests[0].max_redirects, 0);
    EXPECT_FALSE(http.requests[0].allow_insecure_redirect);
    EXPECT_GT(http.requests[0].min_bytes_per_second, 0u);
    EXPECT_GT(http.requests[0].stall_seconds, 0.f);
    EXPECT_EQ(http.requests[0].sink_thread, engine::HttpSinkThread::Worker);
}

TEST(HttpDownloader, TheBodyIsStreamedIntoTheOutputFile) {
    const TempDir dir;
    FakeHttp http;
    http.result = response(200, "archive bytes");
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://github.com/a/b", .output = dir.path() / "o.bin"});
    EXPECT_TRUE(downloader.busy());
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_TRUE(answer->ok());
    EXPECT_EQ(answer->status, 200);
    EXPECT_EQ(launcher_test::read_file(dir.path() / "o.bin"), "archive bytes");
    EXPECT_FALSE(downloader.busy());
    EXPECT_EQ(downloader.poll(), std::nullopt) << "answers once";
}

TEST(HttpDownloader, TheRequestCarriesTheHeadersAndTheBearerToken) {
    const TempDir dir;
    FakeHttp http;
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://api.github.com/x", .output = dir.path() / "b.json",
            .headers = {"Accept: application/vnd.github+json"}, .bearer_token = "ghp_secret", .download = false});
    ASSERT_EQ(http.requests.size(), 1u);
    const auto& headers = http.requests[0].headers;
    const auto has = [&](std::string_view name, std::string_view value) {
        return std::ranges::any_of(
                headers, [&](const engine::HttpHeader& h) { return h.name == name && h.value == value; });
    };
    EXPECT_TRUE(has("Accept", "application/vnd.github+json"));
    EXPECT_TRUE(has("Authorization", "Bearer ghp_secret"));
}

TEST(HttpDownloader, AnswersWithTheStatusAndWritesTheHeaderDump) {
    const TempDir dir;
    FakeHttp http;
    http.result = response(304, {}, {{"ETag", "W/\"abc\""}});
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://api.github.com/x", .output = dir.path() / "b.json",
            .header_output = dir.path() / "h.txt", .download = false});
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_TRUE(answer->ok()) << "an API call reads its 304 itself";
    EXPECT_EQ(answer->status, 304);
    const std::string dump = launcher_test::read_file(dir.path() / "h.txt");
    EXPECT_EQ(launcher::parse_http_status(dump), 304);
    EXPECT_EQ(launcher::find_header(dump, "etag"), "W/\"abc\"");
}

TEST(HttpDownloader, AnErrorPageOfAnApiCallLandsInTheOutputFile) {
    const TempDir dir;
    FakeHttp http;
    http.result = response(403, "{\"message\":\"rate limit\"}");
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{
            .url = "https://api.github.com/x", .output = dir.path() / "b.json", .download = false});
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_TRUE(answer->ok());
    EXPECT_EQ(answer->status, 403);
    EXPECT_EQ(launcher_test::read_file(dir.path() / "b.json"), "{\"message\":\"rate limit\"}");
}

TEST(HttpDownloader, AnHttpErrorOfADownloadIsAnError) {
    const TempDir dir;
    FakeHttp http;
    http.result = response(404, "not found");
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://github.com/x", .output = dir.path() / "o.bin"});
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_FALSE(answer->ok());
    EXPECT_EQ(answer->status, 404);
    EXPECT_NE(answer->error.find("404"), std::string::npos);
    EXPECT_EQ(http.requests.size(), 1u) << "not retried";
}

TEST(HttpDownloader, ABrokenTransferIsStartedAgain) {
    const TempDir dir;
    FakeHttp http;
    http.results = {std::unexpected(engine::HttpError::ConnectionReset), std::unexpected(engine::HttpError::Timeout)};
    http.result = response(200, "done");
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://github.com/x", .output = dir.path() / "o.bin"});
    EXPECT_EQ(downloader.poll(), std::nullopt) << "the first attempt broke: no answer yet";
    EXPECT_TRUE(downloader.busy());
    EXPECT_EQ(downloader.poll(), std::nullopt);
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_TRUE(answer->ok());
    EXPECT_EQ(http.requests.size(), 3u);
    EXPECT_EQ(launcher_test::read_file(dir.path() / "o.bin"), "done");
    EXPECT_FALSE(downloader.busy());
}

TEST(HttpDownloader, ItGivesUpAfterThreeAttempts) {
    const TempDir dir;
    FakeHttp http;
    http.result = std::unexpected(engine::HttpError::Dns);
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://github.com/x", .output = dir.path() / "o.bin"});
    const auto answer = drain(downloader);
    ASSERT_TRUE(answer.has_value());
    EXPECT_FALSE(answer->ok());
    EXPECT_NE(answer->error.find("resolve"), std::string::npos);
    EXPECT_EQ(http.requests.size(), 3u);
}

TEST(HttpDownloader, ARefusedCertificateAndARedirectToHttpAreNotRetried) {
    for (const engine::HttpError error : {engine::HttpError::Tls, engine::HttpError::RedirectRefused}) {
        const TempDir dir;
        FakeHttp http;
        http.result = std::unexpected(error);
        launcher::HttpDownloader downloader(http);
        downloader.start(launcher::DownloadRequest{.url = "https://github.com/x", .output = dir.path() / "o.bin"});
        const auto answer = downloader.poll();
        ASSERT_TRUE(answer.has_value());
        EXPECT_FALSE(answer->ok());
        EXPECT_EQ(http.requests.size(), 1u);
    }
}

TEST(HttpDownloader, APlatformWithoutAnHttpClientSaysSo) {
    const TempDir dir;
    FakeHttp http;
    http.result = std::unexpected(engine::HttpError::Unsupported);
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://github.com/x", .output = dir.path() / "o.bin"});
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_NE(answer->error.find("no HTTP client"), std::string::npos);
    EXPECT_EQ(http.requests.size(), 1u);
}

TEST(HttpDownloader, AnAddressThatIsNotHttpsIsNeverSent) {
    const TempDir dir;
    FakeHttp http;
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "http://github.com/x", .output = dir.path() / "o.bin"});
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_FALSE(answer->ok());
    EXPECT_TRUE(http.requests.empty());
}

TEST(HttpDownloader, AnOutputFileThatCannotBeOpenedIsAnError) {
    const TempDir dir;
    FakeHttp http;
    launcher::HttpDownloader downloader(http);
    downloader.start(
            launcher::DownloadRequest{.url = "https://github.com/x", .output = dir.path() / "missing" / "o.bin"});
    const auto answer = downloader.poll();
    ASSERT_TRUE(answer.has_value());
    EXPECT_FALSE(answer->ok());
    EXPECT_NE(answer->error.find("written"), std::string::npos);
}

TEST(HttpDownloader, AnOldOutputFileIsReplacedBeforeTheTransfer) {
    const TempDir dir;
    dir.write("o.bin", "old and long");
    FakeHttp http;
    http.result = response(200, "new");
    launcher::HttpDownloader downloader(http);
    downloader.start(launcher::DownloadRequest{.url = "https://github.com/x", .output = dir.path() / "o.bin"});
    ASSERT_TRUE(downloader.poll().has_value());
    EXPECT_EQ(launcher_test::read_file(dir.path() / "o.bin"), "new");
}

TEST(HttpDownloader, NothingStartedAnswersNothing) {
    FakeHttp http;
    launcher::HttpDownloader downloader(http);
    EXPECT_FALSE(downloader.busy());
    EXPECT_EQ(downloader.poll(), std::nullopt);
    downloader.cancel();
}
