#pragma once

#include <engine/process/process_desc.h>
#include <engine/project/sdk_manifest.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

// One installed editor SDK the launcher can start.
struct SdkEntry {
    std::filesystem::path root;
    engine::SdkManifest manifest;
    // Remembered by Locate (and forgettable), not found under the install directory. Only an installed SDK can be
    // deleted.
    bool located = false;
};

// The marker file of a symbols package, at the SDK root once the package is unpacked over it.
inline constexpr char kSymbolsMarkerFile[] = "symbols.toml";

// symbols.toml: which SDK build the debug symbols belong to.
struct SymbolsMarker {
    std::string version;
    std::string build_id;
};

// Reads `<directory>/symbols.toml` (flat `key = "value"` lines). nullopt when it is missing, unreadable, or lacks
// `version` or `build_id`.
[[nodiscard]] std::optional<SymbolsMarker> read_symbols_marker(const std::filesystem::path& directory);

// Whether the debug symbols of this very build are over the SDK: the marker's version and build_id are the SDK's.
[[nodiscard]] bool has_symbols(const SdkEntry& sdk);

// Where SDKs are installed: `%LOCALAPPDATA%/Programs/Wind/Sdks` on Windows, `$XDG_DATA_HOME/Wind/Sdks` (or
// `~/.local/share/Wind/Sdks`) elsewhere. Empty when that variable is not set.
[[nodiscard]] std::filesystem::path sdk_install_directory();

// Every SDK of `install_dir/*/sdk.toml`, then every `located` root, without duplicates. Newest version first; for one
// version a clean SDK before a dirty one. A located root without a readable sdk.toml is skipped and described in
// `problems`. Directories whose name starts with '.' are not SDKs (a delete in progress is one).
[[nodiscard]] std::vector<SdkEntry> find_sdks(const std::filesystem::path& install_dir,
        const std::vector<std::filesystem::path>& located, std::vector<std::string>& problems);

// The SDK a project of engine `version` opens with: the first of `sdks` (already in preference order) with exactly that
// version, or nullptr.
[[nodiscard]] const SdkEntry* sdk_for(const std::vector<SdkEntry>& sdks, std::string_view version);

// Order of two engine versions (engine::EngineVersion: 0.1.99a < 0.1.100a < 0.2.0a < 0.2.0b < 0.2.0, a -dev build before
// the releases of its number). A text that is not a version is older than every version, and two of them compare as
// text, so they sort last and are not lost. Negative, zero, or positive like strcmp.
[[nodiscard]] int compare_versions(std::string_view a, std::string_view b);

// Deletes the installed SDK at `root` with its files. It is first renamed to `.deleting-<name>` beside itself, so an
// SDK whose files are in use (an editor running from it) is left whole, and a delete that stops halfway leaves a
// directory find_sdks skips. The error says why in a sentence.
[[nodiscard]] std::expected<void, std::string> delete_sdk(const std::filesystem::path& root);

// Removes what a delete_sdk that stopped halfway left in `install_dir`. Whatever is still in use stays.
void remove_deleted_sdks(const std::filesystem::path& install_dir);

// `<sdk>/bin/wind_editor(.exe) --project <project>`, started in `<sdk>/bin`.
[[nodiscard]] std::filesystem::path editor_executable(const SdkEntry& sdk);
[[nodiscard]] engine::ProcessDesc editor_launch(const SdkEntry& sdk, const std::filesystem::path& project);

// The platform's file manager showing `directory`: Explorer on Windows, `open` on macOS, `xdg-open` elsewhere.
[[nodiscard]] engine::ProcessDesc folder_launch(const std::filesystem::path& directory);

}
