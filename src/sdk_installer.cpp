#include "sdk_installer.h"

#include "launcher_state.h"

#include <engine/project/sdk_manifest.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

constexpr char kWorkPrefix[] = ".installing-";
// The bytes of the archive hashed per frame: a few milliseconds.
constexpr std::size_t kHashSlice = 8u << 20;

// What `source` holds, copied over `target`: directories are made, files replaced. The marker file goes last, so an
// interrupted copy never leaves an SDK that claims to have its symbols.
std::string copy_over(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(source, error);
    for (const std::filesystem::recursive_directory_iterator end; !error && it != end; it.increment(error)) {
        const std::filesystem::directory_entry& entry = *it;
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), source, error);
        if (error) {
            break;
        }
        if (entry.is_directory(error)) {
            std::filesystem::create_directories(target / relative, error);
        } else if (entry.is_regular_file(error) && relative != kSymbolsMarkerFile) {
            std::filesystem::create_directories((target / relative).parent_path(), error);
            std::filesystem::copy_file(entry.path(), target / relative,
                    std::filesystem::copy_options::overwrite_existing, error);
        }
        if (error) {
            return "Could not copy " + path_text(relative) + " into " + path_text(target) + ": " + error.message();
        }
    }
    if (error) {
        return "Could not read the unpacked symbols: " + error.message();
    }
    std::filesystem::copy_file(source / kSymbolsMarkerFile, target / kSymbolsMarkerFile,
            std::filesystem::copy_options::overwrite_existing, error);
    return error ? "Could not copy " + std::string(kSymbolsMarkerFile) + " into " + path_text(target) + ": " +
                           error.message()
                 : std::string{};
}

}

std::filesystem::path installed_sdk_directory(const std::filesystem::path& install_dir, std::string_view version) {
    return install_dir / ("sdk-" + std::string(version));
}

void remove_interrupted_installs(const std::filesystem::path& install_dir) {
    std::error_code error;
    if (install_dir.empty() || !std::filesystem::is_directory(install_dir, error)) {
        return;
    }
    for (const std::filesystem::directory_entry& child : std::filesystem::directory_iterator(install_dir, error)) {
        if (child.is_directory(error) && child.path().filename().string().starts_with(kWorkPrefix)) {
            std::filesystem::remove_all(child.path(), error);
        }
    }
}

SdkInstaller::SdkInstaller(IDownloader& downloader, IExtractor& extractor, Platform platform)
    : downloader_(&downloader)
    , extractor_(&extractor)
    , platform_(std::move(platform)) {}

std::expected<void, std::string> SdkInstaller::start(
        const SdkRelease& release, const std::filesystem::path& install_dir, InstallOptions options) {
    if (running()) {
        return std::unexpected("Another SDK is being installed. Wait for it or cancel it.");
    }
    if (install_dir.empty()) {
        return std::unexpected("There is no install directory on this machine.");
    }
    if (options.symbols && !release.symbols) {
        return std::unexpected("SDK " + release.version + " has no debug symbols for this system.");
    }
    std::error_code error;
    if (std::filesystem::exists(installed_sdk_directory(install_dir, release.version), error)) {
        return std::unexpected("SDK " + release.version + " is installed already.");
    }
    const std::filesystem::path work = install_dir / (kWorkPrefix + release.version);
    std::filesystem::remove_all(work, error);
    std::filesystem::create_directories(work / "download", error);
    if (error) {
        return std::unexpected("Cannot create " + work.generic_string() + ": " + error.message());
    }

    release_ = release;
    install_dir_ = install_dir;
    work_dir_ = work;
    symbols_wanted_ = options.symbols;
    add_to_.clear();
    outcome_.reset();
    begin_package(SdkPackage{.name = release.archive_name,
                          .url = release.archive_url,
                          .checksum_url = release.checksum_url,
                          .size = release.size},
            false);
    return {};
}

std::expected<void, std::string> SdkInstaller::add_symbols(
        const SdkRelease& release, const std::filesystem::path& sdk_root) {
    if (running()) {
        return std::unexpected("Another SDK is being installed. Wait for it or cancel it.");
    }
    if (!release.symbols) {
        return std::unexpected("SDK " + release.version + " has no debug symbols for this system.");
    }
    const auto manifest = engine::read_sdk_manifest(sdk_root);
    if (!manifest || manifest->version != release.version) {
        return std::unexpected(path_text(sdk_root) + " is not SDK " + release.version + ".");
    }
    const std::optional<SymbolsMarker> marker = read_symbols_marker(sdk_root);
    if (marker && marker->version == manifest->version && marker->build_id == manifest->build_id) {
        return std::unexpected("SDK " + release.version + " has its debug symbols already.");
    }
    const std::filesystem::path install_dir = sdk_root.parent_path();
    const std::filesystem::path work = install_dir / (std::string(kWorkPrefix) + release.version + "-symbols");
    std::error_code error;
    std::filesystem::remove_all(work, error);
    std::filesystem::create_directories(work / "download", error);
    if (error) {
        return std::unexpected("Cannot create " + work.generic_string() + ": " + error.message());
    }

    release_ = release;
    install_dir_ = install_dir;
    work_dir_ = work;
    symbols_wanted_ = false;
    add_to_ = sdk_root;
    outcome_.reset();
    begin_package(*release.symbols, true);
    return {};
}

void SdkInstaller::begin_package(const SdkPackage& package, bool symbols) {
    package_ = package;
    archive_path_ = work_dir_ / "download" / package.name;
    expected_hash_.clear();
    hasher_.reset();
    progress_ = InstallProgress{.phase = InstallPhase::Checksum, .version = release_.version, .symbols = symbols};

    DownloadRequest request;
    request.url = package.checksum_url;
    request.output = work_dir_ / "download" / (package.name + ".sha256");
    downloader_->start(std::move(request));
}

void SdkInstaller::poll() {
    switch (progress_.phase) {
        case InstallPhase::Idle:
            return;
        case InstallPhase::Checksum:
            if (const std::optional<DownloadResult> answer = downloader_->poll()) {
                advance_checksum(*answer);
            }
            return;
        case InstallPhase::Download: {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(archive_path_, error);
            if (!error) {
                progress_.done = size;
            }
            if (const std::optional<DownloadResult> answer = downloader_->poll()) {
                advance_download(*answer);
            }
            return;
        }
        case InstallPhase::Verify:
            verify_slice();
            return;
        case InstallPhase::Unpack:
            if (const std::optional<ExtractResult> answer = extractor_->poll()) {
                advance_unpack(*answer);
            }
            return;
    }
}

void SdkInstaller::advance_checksum(const DownloadResult& answer) {
    if (!answer.ok()) {
        fail("Could not download the checksum: " + answer.error);
        return;
    }
    expected_hash_ = parse_checksum_file(read_text_file(work_dir_ / "download" / (package_.name + ".sha256")));
    if (expected_hash_.empty()) {
        fail("The checksum file of " + package_.name + " is not a SHA-256.");
        return;
    }
    progress_.phase = InstallPhase::Download;
    progress_.done = 0;
    progress_.total = package_.size;
    DownloadRequest request;
    request.url = package_.url;
    request.output = archive_path_;
    downloader_->start(std::move(request));
}

void SdkInstaller::advance_download(const DownloadResult& answer) {
    if (!answer.ok()) {
        fail("Could not download " + package_.name + ": " + answer.error);
        return;
    }
    hasher_ = std::make_unique<FileHasher>(archive_path_);
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(archive_path_, error);
    progress_.phase = InstallPhase::Verify;
    progress_.done = 0;
    progress_.total = error ? 0 : size;
}

void SdkInstaller::verify_slice() {
    if (!hasher_->step(kHashSlice)) {
        progress_.done = std::min<std::uint64_t>(progress_.total, progress_.done + kHashSlice);
        return;
    }
    if (hasher_->failed()) {
        fail("Could not read the downloaded " + package_.name + ".");
        return;
    }
    if (hasher_->hex() != expected_hash_) {
        fail("The download of " + package_.name + " is damaged: its SHA-256 is not the published one.");
        return;
    }
    hasher_.reset();
    progress_.phase = InstallPhase::Unpack;
    const bool symbols = progress_.symbols;
    extractor_->start(ExtractRequest{.archive = archive_path_,
            .destination = work_dir_ / (symbols ? "symbols" : "extract"),
            .top_directory = symbols ? symbols_top_directory(release_.version, platform_)
                                     : sdk_top_directory(release_.version, platform_)});
}

void SdkInstaller::advance_unpack(const ExtractResult& answer) {
    if (!answer.ok()) {
        fail("Could not unpack " + package_.name + ": " + answer.error);
        return;
    }
    // The archive is not needed any more; the next package should not pile up on it.
    std::error_code error;
    std::filesystem::remove(archive_path_, error);
    if (progress_.symbols) {
        finish_symbols();
    } else {
        finish_sdk();
    }
}

void SdkInstaller::finish_sdk() {
    const std::filesystem::path unpacked = work_dir_ / "extract" / sdk_top_directory(release_.version, platform_);
    const auto manifest = engine::read_sdk_manifest(unpacked);
    if (!manifest) {
        fail("The SDK in " + package_.name + " has no readable sdk.toml: " + engine::describe(manifest.error()));
        return;
    }
    if (manifest->version != release_.version) {
        fail("The SDK in " + package_.name + " is version " + manifest->version + ", not " + release_.version + ".");
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(editor_executable(SdkEntry{.root = unpacked}), error)) {
        fail("The SDK in " + package_.name + " has no editor (bin/wind_editor).");
        return;
    }
    if (symbols_wanted_ && release_.symbols) {
        begin_package(*release_.symbols, true);
        return;
    }
    commit_install({});
}

void SdkInstaller::finish_symbols() {
    const std::filesystem::path unpacked =
            work_dir_ / "symbols" / symbols_top_directory(release_.version, platform_);
    const std::filesystem::path target = add_to_.empty()
            ? work_dir_ / "extract" / sdk_top_directory(release_.version, platform_)
            : add_to_;
    const std::optional<SymbolsMarker> marker = read_symbols_marker(unpacked);
    if (!marker) {
        fail_symbols("The debug symbols in " + package_.name + " have no readable " + kSymbolsMarkerFile + ".");
        return;
    }
    if (marker->version != release_.version) {
        fail_symbols("The debug symbols in " + package_.name + " are version " + marker->version + ", not " +
                     release_.version + ".");
        return;
    }
    const auto manifest = engine::read_sdk_manifest(target);
    if (!manifest) {
        fail_symbols("The SDK in " + path_text(target) + " has no readable sdk.toml: " +
                     engine::describe(manifest.error()));
        return;
    }
    if (manifest->build_id.empty() || marker->build_id != manifest->build_id) {
        fail_symbols("The debug symbols in " + package_.name + " are of another build than the SDK.");
        return;
    }
    if (std::string problem = copy_over(unpacked, target); !problem.empty()) {
        fail_symbols(std::move(problem));
        return;
    }
    if (add_to_.empty()) {
        commit_install(" It has its debug symbols.");
        return;
    }
    end(InstallOutcome{.version = release_.version,
            .installed = true,
            .message = "Installed the debug symbols of SDK " + release_.version + "."});
}

void SdkInstaller::commit_install(std::string_view note) {
    // Every package is in: a failure from here on is the install's own, not a package's.
    progress_.symbols = false;
    const std::filesystem::path unpacked = work_dir_ / "extract" / sdk_top_directory(release_.version, platform_);
    std::error_code error;
    const std::filesystem::path target = installed_sdk_directory(install_dir_, release_.version);
    if (std::filesystem::exists(target, error)) {
        fail("SDK " + release_.version + " is installed already.");
        return;
    }
    // One rename inside the install directory: the SDK appears whole or not at all.
    std::filesystem::rename(unpacked, target, error);
    if (error) {
        fail("Could not move the SDK to " + target.generic_string() + ": " + error.message());
        return;
    }
    std::filesystem::remove_all(work_dir_, error);
    end(InstallOutcome{.version = release_.version,
            .installed = true,
            .message = "Installed SDK " + release_.version + "." + std::string(note)});
}

void SdkInstaller::fail_symbols(std::string message) {
    if (!add_to_.empty()) {
        fail(std::move(message));
        return;
    }
    // The SDK itself is good: it is installed, without the optional package.
    commit_install(" Its debug symbols were not installed: " + message + " Add them from the SDKs page.");
}

void SdkInstaller::fail(std::string message) {
    // A failed package after the SDK is its own case only when symbols were asked for along with it (see fail_symbols).
    if (progress_.symbols && add_to_.empty()) {
        fail_symbols(std::move(message));
        return;
    }
    end(InstallOutcome{.version = release_.version, .message = std::move(message)});
}

void SdkInstaller::end(InstallOutcome outcome) {
    // Whatever was running ends, and the temporary directory goes with the partial download.
    downloader_->cancel();
    extractor_->cancel();
    hasher_.reset();
    std::error_code error;
    std::filesystem::remove_all(work_dir_, error);
    progress_ = InstallProgress{};
    outcome_ = std::move(outcome);
}

void SdkInstaller::cancel() {
    if (!running()) {
        return;
    }
    end(InstallOutcome{.version = release_.version, .cancelled = true, .message = "Install of SDK " + release_.version + " cancelled."});
}

std::optional<InstallOutcome> SdkInstaller::take_outcome() {
    return std::exchange(outcome_, std::nullopt);
}

}
