#pragma once

#include <engine/net/http_call.h>
#include <engine/net/http_client.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// One HTTPS GET into a file.
struct DownloadRequest {
    std::string url;
    // The body goes here. The directory exists.
    std::filesystem::path output;
    // The response headers go here when it is not empty (the last response, after redirects).
    std::filesystem::path header_output;
    // `Name: value` lines sent with the request.
    std::vector<std::string> headers;
    // A bearer token for the request, or empty.
    std::string bearer_token;
    // A download: follow redirects, and an HTTP status of 400 or more is an error. Off for an API call, whose 304,
    // 403, and 429 answers the caller reads itself.
    bool download = true;
};

struct DownloadResult {
    // The HTTP status of the last response, 0 when none arrived.
    int status = 0;
    // Why nothing was received, or empty. A status of 400 or more of a `download` is an error too.
    std::string error;

    [[nodiscard]] bool ok() const {
        return error.empty();
    }
};

// Fetches one URL at a time without blocking the frame: start(), then poll() every frame until it answers. Main thread
// only.
class IDownloader {
public:
    virtual ~IDownloader() = default;

    [[nodiscard]] virtual bool busy() const = 0;
    virtual void start(DownloadRequest request) = 0;
    // The answer, once, after the transfer ended; nullopt while it runs or when nothing is started.
    [[nodiscard]] virtual std::optional<DownloadResult> poll() = 0;
    // Ends the transfer; no answer follows. The partial output file is left to the caller.
    virtual void cancel() = 0;
};

// The engine's HttpClient. The body of a 2xx answer is streamed into `output` on a worker thread, so an archive never
// sits in memory. A transfer that breaks (a reset connection, a timeout, a host that does not resolve) is started again,
// up to three times in all. A redirect to http, an HTTP error (a 404), and a platform without an HTTP client are not
// retried.
class HttpDownloader final : public IDownloader {
public:
    explicit HttpDownloader(engine::IHttpClient& http);

    [[nodiscard]] bool busy() const override;
    void start(DownloadRequest request) override;
    [[nodiscard]] std::optional<DownloadResult> poll() override;
    void cancel() override;

private:
    struct Sink;

    void send();
    [[nodiscard]] DownloadResult finish(const engine::HttpResult& result);

    engine::IHttpClient* http_;
    engine::HttpCall call_;
    DownloadRequest request_;
    std::shared_ptr<Sink> sink_;
    int attempts_ = 0;
    // From start() until poll() has given the answer or cancel(): a finished call is not pending, but is not answered yet.
    bool active_ = false;
};

// The `Name: value` lines of a request as headers; a line without a colon is dropped.
[[nodiscard]] std::vector<engine::HttpHeader> parse_header_lines(const std::vector<std::string>& lines);

// A header dump of a response: `HTTP/1.1 <status>` and a `Name: value` line per header, ended by a blank line, the form
// parse_http_status and find_header read.
[[nodiscard]] std::string format_header_dump(const engine::HttpResponse& response);

// The status of the last `HTTP/<v> <code> ...` line of a header dump, 0 when there is none.
[[nodiscard]] int parse_http_status(std::string_view header_dump);

// The value of the header `name` (case-insensitive) in the last response of a header dump, or nullopt.
[[nodiscard]] std::optional<std::string> find_header(std::string_view header_dump, std::string_view name);

// The whole file as text, or empty when it cannot be read.
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);

}
