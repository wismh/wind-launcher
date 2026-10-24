#include "downloader.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <expected>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The lines of the last response in a header dump: after the last status line, up to the blank line.
std::vector<std::string_view> last_response(std::string_view dump) {
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos <= dump.size()) {
        std::size_t end = dump.find('\n', pos);
        if (end == std::string_view::npos) {
            end = dump.size();
        }
        std::string_view line = dump.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with("HTTP/")) {
            lines.clear();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
        pos = end + 1;
    }
    return lines;
}

}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int parse_http_status(std::string_view header_dump) {
    const std::vector<std::string_view> lines = last_response(header_dump);
    if (lines.empty() || !lines.front().starts_with("HTTP/")) {
        return 0;
    }
    const std::size_t space = lines.front().find(' ');
    if (space == std::string_view::npos) {
        return 0;
    }
    int status = 0;
    for (std::size_t i = space + 1; i < lines.front().size() && std::isdigit(static_cast<unsigned char>(lines.front()[i]));
            ++i) {
        status = status * 10 + (lines.front()[i] - '0');
    }
    return status;
}

std::optional<std::string> find_header(std::string_view header_dump, std::string_view name) {
    const std::string wanted = lower(name);
    for (const std::string_view line : last_response(header_dump)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || lower(line.substr(0, colon)) != wanted) {
            continue;
        }
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        return std::string(value);
    }
    return std::nullopt;
}

std::vector<engine::HttpHeader> parse_header_lines(const std::vector<std::string>& lines) {
    std::vector<engine::HttpHeader> headers;
    for (const std::string& line : lines) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string_view value = std::string_view(line).substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        headers.push_back({.name = line.substr(0, colon), .value = std::string(value)});
    }
    return headers;
}

std::string format_header_dump(const engine::HttpResponse& response) {
    std::string dump = "HTTP/1.1 " + std::to_string(response.status) + "\r\n";
    for (const engine::HttpHeader& header : response.headers) {
        dump += header.name + ": " + header.value + "\r\n";
    }
    dump += "\r\n";
    return dump;
}

// The body of a 2xx answer, written as the bytes arrive. The sink runs on the transfer's worker thread, and the engine
// does not call it again once HttpCall::cancel or the call's destructor has returned, so the file is closed only after.
struct HttpDownloader::Sink {
    std::ofstream file;
    // Set on the worker thread when a write failed; read after the answer, which orders the two.
    bool failed = false;
};

HttpDownloader::HttpDownloader(engine::IHttpClient& http)
    : http_(&http) {}

bool HttpDownloader::busy() const {
    return active_;
}

void HttpDownloader::start(DownloadRequest request) {
    request_ = std::move(request);
    std::error_code error;
    if (!request_.header_output.empty()) {
        std::filesystem::remove(request_.header_output, error);
    }
    attempts_ = 0;
    active_ = true;
    send();
}

void HttpDownloader::send() {
    ++attempts_;
    sink_ = std::make_shared<Sink>();
    // Opening truncates: an old file, or the part of a transfer that broke, is gone.
    sink_->file.open(request_.output, std::ios::binary | std::ios::trunc);

    engine::HttpRequest http;
    http.url = request_.url;
    http.headers = parse_header_lines(request_.headers);
    if (!request_.bearer_token.empty()) {
        http.headers.push_back({.name = "Authorization", .value = "Bearer " + request_.bearer_token});
    }
    if (request_.download) {
        // A release asset is served from another host, so follow redirects, but never to http. Give up on a transfer
        // that stalls for a minute.
        http.timeout_seconds = 30.f;
        http.max_redirects = 10;
        http.allow_insecure_redirect = false;
        http.min_bytes_per_second = 1000;
        http.stall_seconds = 60.f;
    } else {
        // An API call: its 304, 403, and 429 answers are the caller's to read.
        http.timeout_seconds = 60.f;
        http.max_redirects = 0;
    }
    http.sink = [sink = sink_](std::span<const std::byte> chunk) {
        sink->file.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        sink->failed = sink->failed || !sink->file;
        return !sink->failed;
    };
    http.sink_thread = engine::HttpSinkThread::Worker;
    if (!sink_->file.is_open() || !request_.url.starts_with("https://")) {
        // No transfer: the answer is the failure, from the first poll.
        sink_->failed = !sink_->file.is_open();
        call_ = engine::HttpCall::resolved(std::unexpected(engine::HttpError::InvalidUrl));
        return;
    }
    call_ = http_->send(std::move(http));
}

namespace {

bool retryable(engine::HttpError error) {
    switch (error) {
    case engine::HttpError::Network:
    case engine::HttpError::Timeout:
    case engine::HttpError::Dns:
    case engine::HttpError::ConnectionReset:
        return true;
    case engine::HttpError::InvalidUrl:
    case engine::HttpError::Unsupported:
    case engine::HttpError::TooLarge:
    case engine::HttpError::Tls:
    case engine::HttpError::RedirectRefused:
    case engine::HttpError::SinkAborted:
        return false;
    }
    return false;
}

std::string describe(engine::HttpError error) {
    switch (error) {
    case engine::HttpError::InvalidUrl:
        return "the address is not an https link";
    case engine::HttpError::Unsupported:
        return "this platform has no HTTP client (on Linux and macOS Wind needs libcurl installed)";
    case engine::HttpError::Network:
        return "the network request failed";
    case engine::HttpError::Timeout:
        return "the connection timed out";
    case engine::HttpError::TooLarge:
        return "the answer is too large";
    case engine::HttpError::Dns:
        return "could not resolve the host";
    case engine::HttpError::Tls:
        return "the TLS handshake or certificate was refused";
    case engine::HttpError::ConnectionReset:
        return "the connection was dropped";
    case engine::HttpError::RedirectRefused:
        return "a redirect was refused (too many, or to http)";
    case engine::HttpError::SinkAborted:
        return "the file could not be written";
    }
    return "the network request failed";
}

}

std::optional<DownloadResult> HttpDownloader::poll() {
    const std::optional<engine::HttpResult> result = call_.take();
    if (!result) {
        return std::nullopt;
    }
    constexpr int kMaxAttempts = 3;
    if (!*result && retryable(result->error()) && attempts_ < kMaxAttempts) {
        send();
        return std::nullopt;
    }
    active_ = false;
    return finish(*result);
}

DownloadResult HttpDownloader::finish(const engine::HttpResult& result) {
    const bool write_failed = sink_->failed || (sink_->file.is_open() && !sink_->file);
    if (sink_->file.is_open()) {
        sink_->file.close();
    }
    DownloadResult answer;
    if (!result) {
        answer.error = write_failed ? describe(engine::HttpError::SinkAborted) : describe(result.error());
        return answer;
    }
    const engine::HttpResponse& response = *result;
    answer.status = response.status;
    if (!request_.header_output.empty()) {
        std::ofstream out(request_.header_output, std::ios::binary | std::ios::trunc);
        out << format_header_dump(response);
    }
    const bool success = response.status >= 200 && response.status < 300;
    if (!success && !response.body.empty()) {
        // Only a 2xx body went to the sink: an error page is written here, for a caller that reads it.
        std::ofstream out(request_.output, std::ios::binary | std::ios::trunc);
        out << response.body;
    }
    if (write_failed) {
        answer.error = describe(engine::HttpError::SinkAborted);
    } else if (request_.download && response.status >= 400) {
        answer.error = "the server answered " + std::to_string(response.status);
    }
    return answer;
}

void HttpDownloader::cancel() {
    active_ = false;
    // Cancelling waits for a sink that is running, so the file can be closed after it.
    call_.cancel();
    if (sink_ && sink_->file.is_open()) {
        sink_->file.close();
    }
}

}
