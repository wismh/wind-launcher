#include "sdk_catalog.h"

#include "launcher_state.h"
#include "user_paths.h"

#include <engine/project/engine_version.h>

#include <algorithm>
#include <compare>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace launcher {
namespace {

constexpr std::string_view kDeletingPrefix = ".deleting-";

bool hidden(const std::filesystem::path& directory) {
    return path_text(directory.filename()).starts_with('.');
}

bool known(const std::vector<SdkEntry>& sdks, const std::filesystem::path& root) {
    return std::ranges::any_of(sdks, [&](const SdkEntry& entry) { return same_directory(entry.root, root); });
}

// The value of `key = "value"` among the top-level lines of a flat TOML text.
std::optional<std::string> string_value(std::string_view text, std::string_view key) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos || line[first] == '#') {
            continue;
        }
        line.remove_prefix(first);
        if (line.front() == '[') {
            return std::nullopt;
        }
        if (!line.starts_with(key)) {
            continue;
        }
        line.remove_prefix(key.size());
        const std::size_t equals = line.find_first_not_of(" \t");
        if (equals == std::string_view::npos || line[equals] != '=') {
            continue;
        }
        line.remove_prefix(equals + 1);
        const std::size_t open = line.find('"');
        const std::size_t close = open == std::string_view::npos ? open : line.find('"', open + 1);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        return std::string(line.substr(open + 1, close - open - 1));
    }
    return std::nullopt;
}

}

std::optional<SymbolsMarker> read_symbols_marker(const std::filesystem::path& directory) {
    std::ifstream in(directory / kSymbolsMarkerFile, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::optional<std::string> version = string_value(text, "version");
    std::optional<std::string> build_id = string_value(text, "build_id");
    if (!version || !build_id) {
        return std::nullopt;
    }
    return SymbolsMarker{.version = std::move(*version), .build_id = std::move(*build_id)};
}

bool has_symbols(const SdkEntry& sdk) {
    const std::optional<SymbolsMarker> marker = read_symbols_marker(sdk.root);
    return marker && marker->version == sdk.manifest.version && !sdk.manifest.build_id.empty() &&
           marker->build_id == sdk.manifest.build_id;
}

int compare_versions(std::string_view a, std::string_view b) {
    const std::optional<engine::EngineVersion> left = engine::parse_engine_version(a);
    const std::optional<engine::EngineVersion> right = engine::parse_engine_version(b);
    if (left && right) {
        const std::strong_ordering order = *left <=> *right;
        return order < 0 ? -1 : (order > 0 ? 1 : 0);
    }
    if (left || right) {
        return left ? 1 : -1;
    }
    const int text = a.compare(b);
    return text < 0 ? -1 : (text > 0 ? 1 : 0);
}

std::filesystem::path sdk_install_directory() {
#if defined(_WIN32)
    const std::filesystem::path local = environment_path("LOCALAPPDATA");
    return local.empty() ? local : local / "Programs" / "Wind" / "Sdks";
#else
    if (const std::filesystem::path data = environment_path("XDG_DATA_HOME"); !data.empty()) {
        return data / "Wind" / "Sdks";
    }
    const std::filesystem::path home = environment_path("HOME");
    return home.empty() ? home : home / ".local" / "share" / "Wind" / "Sdks";
#endif
}

std::vector<SdkEntry> find_sdks(const std::filesystem::path& install_dir,
        const std::vector<std::filesystem::path>& located, std::vector<std::string>& problems) {
    std::vector<SdkEntry> sdks;
    std::error_code error;
    if (!install_dir.empty() && std::filesystem::is_directory(install_dir, error)) {
        for (const std::filesystem::directory_entry& child : std::filesystem::directory_iterator(install_dir, error)) {
            if (!child.is_directory(error) || hidden(child.path()) ||
                    !std::filesystem::exists(child.path() / engine::kSdkManifestFile, error)) {
                continue;
            }
            if (auto manifest = engine::read_sdk_manifest(child.path())) {
                sdks.push_back(SdkEntry{.root = child.path(), .manifest = std::move(*manifest), .located = false});
            } else {
                problems.push_back(path_text(child.path()) + ": " + engine::describe(manifest.error()));
            }
        }
    }
    for (const std::filesystem::path& root : located) {
        if (known(sdks, root)) {
            continue;
        }
        if (auto manifest = engine::read_sdk_manifest(root)) {
            sdks.push_back(SdkEntry{.root = root, .manifest = std::move(*manifest), .located = true});
        } else {
            problems.push_back(path_text(root) + ": " + engine::describe(manifest.error()));
        }
    }
    std::ranges::stable_sort(sdks, [](const SdkEntry& a, const SdkEntry& b) {
        if (const int order = compare_versions(a.manifest.version, b.manifest.version); order != 0) {
            return order > 0;
        }
        return !a.manifest.dirty && b.manifest.dirty;
    });
    return sdks;
}

const SdkEntry* sdk_for(const std::vector<SdkEntry>& sdks, std::string_view version) {
    const auto found = std::ranges::find_if(sdks, [&](const SdkEntry& entry) { return entry.manifest.version == version; });
    return found == sdks.end() ? nullptr : &*found;
}

std::expected<void, std::string> delete_sdk(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        return std::unexpected(path_text(root) + " is not there any more.");
    }
    const std::filesystem::path doomed = root.parent_path() / (std::string(kDeletingPrefix) + path_text(root.filename()));
    std::filesystem::remove_all(doomed, error);
    std::filesystem::rename(root, doomed, error);
    if (error) {
        return std::unexpected("Could not delete " + path_text(root) + ": " + error.message() +
                ". Close the editor running from it and try again.");
    }
    std::filesystem::remove_all(doomed, error);
    if (error) {
        return std::unexpected("Deleted the SDK, but some files are still in use (" + error.message() +
                "). The launcher removes " + path_text(doomed) + " when it starts.");
    }
    return {};
}

void remove_deleted_sdks(const std::filesystem::path& install_dir) {
    std::error_code error;
    if (install_dir.empty() || !std::filesystem::is_directory(install_dir, error)) {
        return;
    }
    std::vector<std::filesystem::path> doomed;
    for (const std::filesystem::directory_entry& child : std::filesystem::directory_iterator(install_dir, error)) {
        if (path_text(child.path().filename()).starts_with(kDeletingPrefix)) {
            doomed.push_back(child.path());
        }
    }
    for (const std::filesystem::path& directory : doomed) {
        std::filesystem::remove_all(directory, error);
    }
}

std::filesystem::path editor_executable(const SdkEntry& sdk) {
#if defined(_WIN32)
    return sdk.root / "bin" / "wind_editor.exe";
#else
    return sdk.root / "bin" / "wind_editor";
#endif
}

engine::ProcessDesc editor_launch(const SdkEntry& sdk, const std::filesystem::path& project) {
    return engine::ProcessDesc{
            .program = editor_executable(sdk),
            .arguments = {"--project", path_text(project)},
            .working_directory = sdk.root / "bin",
            .environment = {},
    };
}

engine::ProcessDesc folder_launch(const std::filesystem::path& directory) {
#if defined(_WIN32)
    const std::filesystem::path program = "explorer";
#elif defined(__APPLE__)
    const std::filesystem::path program = "open";
#else
    const std::filesystem::path program = "xdg-open";
#endif
    std::filesystem::path native = directory;
    native.make_preferred();
    const std::u8string text = native.u8string();
    return engine::ProcessDesc{
            .program = program,
            .arguments = {std::string(reinterpret_cast<const char*>(text.data()), text.size())},
            .working_directory = {},
            .environment = {},
    };
}

}
