#pragma once

#include "downloader.h"
#include "extractor.h"
#include "sdk_release.h"
#include "sha256.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace launcher {

enum class InstallPhase {
    Idle,
    // The `.sha256` file.
    Checksum,
    // The archive. `done` of `total` bytes.
    Download,
    // The SHA-256 of the archive. `done` of `total` bytes.
    Verify,
    Unpack,
};

struct InstallProgress {
    InstallPhase phase = InstallPhase::Idle;
    std::string version;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    // Which package the phase is on: the SDK archive, or (true) the debug symbols that follow it or are added later.
    bool symbols = false;
};

// What an install takes besides the SDK itself.
struct InstallOptions {
    // The debug symbols package, unpacked over the SDK before it appears.
    bool symbols = false;
};

// How an install ended.
struct InstallOutcome {
    std::string version;
    bool installed = false;
    bool cancelled = false;
    // For the row: why it failed, or what was done.
    std::string message;
};

// The directory an SDK of `version` is installed in: `<install_dir>/sdk-<version>`.
[[nodiscard]] std::filesystem::path installed_sdk_directory(
        const std::filesystem::path& install_dir, std::string_view version);

// Removes what an install that was interrupted (the launcher closed, the machine lost power) left in `install_dir`:
// the `.installing-*` directories. Whatever is still in use stays.
void remove_interrupted_installs(const std::filesystem::path& install_dir);

// Downloads an SDK release, checks it, and installs it: the checksum file, the archive, its SHA-256, the unpacking into
// a temporary directory inside the install directory, the checks of what was unpacked (the one top directory,
// `sdk.toml` of that version, the editor binary), and a rename to `sdk-<version>`. With `InstallOptions::symbols` the
// debug symbols package goes through the same steps after the SDK and is copied over the unpacked SDK before the
// rename, so the SDK appears whole with them; if only that package fails, the SDK is still installed and the outcome
// says what is missing. add_symbols() does the same steps for an SDK that is installed already. One install at a time.
// Nothing blocks the frame: the transfers and tar run as processes and the hash is computed a slice per poll(). Any
// failure and a cancel remove the temporary directory and leave the SDK list as it was. The archives are never
// executed. Main thread only.
class SdkInstaller {
public:
    SdkInstaller(IDownloader& downloader, IExtractor& extractor, Platform platform = current_platform());

    // Starts installing `release`. The reason when nothing started: another install runs, the SDK is there already, the
    // symbols were asked for and the release has none, or the install directory cannot be used.
    [[nodiscard]] std::expected<void, std::string> start(const SdkRelease& release,
            const std::filesystem::path& install_dir, InstallOptions options = {});
    // Starts adding the debug symbols of `release` to the SDK installed at `sdk_root`. The reason when nothing started:
    // another install runs, the release has no symbols, `sdk_root` is not the SDK of that version, or its symbols are
    // there already.
    [[nodiscard]] std::expected<void, std::string> add_symbols(
            const SdkRelease& release, const std::filesystem::path& sdk_root);
    // Advances the install. Call every frame.
    void poll();
    void cancel();

    [[nodiscard]] bool running() const {
        return progress_.phase != InstallPhase::Idle;
    }
    [[nodiscard]] const InstallProgress& progress() const {
        return progress_;
    }
    // How the last install ended, once.
    [[nodiscard]] std::optional<InstallOutcome> take_outcome();

private:
    // Starts the package: its checksum file first.
    void begin_package(const SdkPackage& package, bool symbols);
    void advance_checksum(const DownloadResult& answer);
    void advance_download(const DownloadResult& answer);
    void verify_slice();
    void advance_unpack(const ExtractResult& answer);
    // The checks of the unpacked SDK; then the symbols package if asked for, else commit_install().
    void finish_sdk();
    // The checks of the unpacked symbols and their copy over the SDK.
    void finish_symbols();
    // Moves the unpacked SDK to `sdk-<version>`. `note` is added to the message.
    void commit_install(std::string_view note);
    // A failed symbols package: the SDK is installed anyway when it is not installed yet.
    void fail_symbols(std::string message);
    void fail(std::string message);
    void end(InstallOutcome outcome);

    IDownloader* downloader_;
    IExtractor* extractor_;
    Platform platform_;

    InstallProgress progress_;
    SdkRelease release_;
    std::filesystem::path install_dir_;
    std::filesystem::path work_dir_;
    std::filesystem::path archive_path_;
    // The package on its way, and what the install still takes after it.
    SdkPackage package_;
    bool symbols_wanted_ = false;
    // Adding the symbols to this installed SDK; empty for an install of the SDK itself.
    std::filesystem::path add_to_;
    std::string expected_hash_;
    std::unique_ptr<FileHasher> hasher_;
    std::optional<InstallOutcome> outcome_;
};

}
