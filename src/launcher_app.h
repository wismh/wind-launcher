#pragma once

#include "available_sdk_row_view_model.h"
#include "downloader.h"
#include "extractor.h"
#include "launcher_request.h"
#include "launcher_state.h"
#include "launcher_view_model.h"
#include "project_entry.h"
#include "project_template.h"
#include "release_feed.h"
#include "sdk_catalog.h"
#include "sdk_installer.h"
#include "sdk_release.h"

#include <engine/core/engine_services.h>
#include <engine/core/file_dialog.h>
#include <engine/igame.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

// Wind Launcher: the projects the user works on and the editor SDKs on this machine, on pages a navbar switches
// between. Open starts the editor whose version the project names, with `--project`; New project copies an SDK's
// project template and opens the result. A Wind app like a game, built against the static engine, so it serves every
// SDK version.
//
// Remembers its lists in `user_data_directory("Wind", "Launcher")/launcher.txt` and finds installed SDKs under
// sdk_install_directory(). The SDKs page also lists the SDKs of the engine's GitHub releases for this platform
// (ReleaseFeed) and installs one on request (SdkInstaller): the downloads go through the engine's HTTP client and
// the unpacking runs as a `tar` process, both polled in the frame system, so the window stays responsive. Holds `this`
// in its rows and its system, so it never moves.
class LauncherApp final : public engine::GameBase {
public:
    explicit LauncherApp(const engine::EngineServices& services);

    LauncherApp(const LauncherApp&) = delete;
    LauncherApp& operator=(const LauncherApp&) = delete;

    [[nodiscard]] engine::WindowDesc primary_window() const override;
    void on_start() override;

    // Navbar and page buttons.
    void show_projects();
    void show_sdks();
    void show_install_sdk();
    // The modules page: back to the list, or install the SDK with what is ticked.
    void close_install_options();
    void confirm_install();
    void add_project();
    void new_project();
    void locate_sdk();
    void refresh_available();
    void show_sdk_folder();

    // The New project page.
    void browse_location();
    void toggle_sdk_picker();
    void pick_new_project_sdk(std::size_t index);
    void create_project();
    [[nodiscard]] bool can_create_project() const;
    void cancel_new_project();

    // Row buttons. The last request since the previous frame wins.
    void request(LauncherRequest request);

private:
    enum class Page {
        Projects,
        Sdks,
        InstallSdk,
        InstallOptions,
        NewProject,
    };

    enum class DialogPurpose {
        AddProject,
        LocateSdk,
        ProjectLocation,
    };

    void frame();
    void show_page(Page page);
    void take_dialog_answer(const engine::FileDialogResult& answer);
    void act(const LauncherRequest& request);
    void open_dialog(DialogPurpose purpose);
    // In `sdk`, or the one sdk_for picks for the project's version.
    void open_project(std::size_t index, const SdkEntry* sdk = nullptr);
    void delete_sdk(std::size_t index);
    // Writes `version` as the engine of project `index` and reads the projects again.
    void change_project_sdk(std::size_t index, const std::string& version);
    // The Available list. Opens the modules page for the row `index`.
    void show_install_options(std::size_t index);
    void start_install(const SdkRelease& release, InstallOptions options = {});
    // Downloads the debug symbols of the installed SDK `index` of sdks_.
    void add_symbols(std::size_t index);
    void finish_install(const InstallOutcome& outcome);
    // Rebuilds the Available rows from the feed, the installed SDKs, and the install in progress.
    void rebuild_available();
    void rebuild_project_rows();
    // Rebuilds the SDKs rows from sdks_, the releases (for the symbols), and the install in progress.
    void rebuild_sdk_rows();
    // Writes the progress of the running install on its row and the note above the list.
    void show_install_progress();
    void update_available_note();
    [[nodiscard]] const AvailableSdk* find_available(std::string_view version) const;
    void show_folder(const std::filesystem::path& directory);
    // The SDK picked on the New project page, or nullptr.
    [[nodiscard]] const SdkEntry* picked_sdk() const;
    // What the New project page would create from its fields and picked SDK.
    [[nodiscard]] NewProject new_project_form() const;
    // Checks the New project fields again and writes the hint. Runs every frame the page is shown: typing has no
    // change event.
    void check_new_project();
    void make_project();
    // The picker's rows from sdks_, keeping the picked root when it is still there.
    void refresh_sdk_options();
    // Reads every project and SDK again, rebuilds the rows, and saves the lists.
    void refresh();
    void show_status(std::string text);

    engine::IWindowControl* windows_;
    engine::IProcessLauncher* processes_;
    std::shared_ptr<LauncherViewModel> view_model_;

    // Two downloaders: the list of releases and an install run side by side.
    HttpDownloader feed_downloader_;
    HttpDownloader install_downloader_;
    TarExtractor extractor_;
    SdkInstaller installer_;
    // Made in on_start, when the user data directory is known.
    std::unique_ptr<ReleaseFeed> feed_;
    unsigned feed_revision_ = 0;
    std::vector<AvailableSdk> available_;
    std::vector<std::shared_ptr<AvailableSdkRowViewModel>> available_rows_;
    // The version the modules page is for.
    std::string options_version_;
    // Why the last install of `version` failed, shown on its row until the next try or the next list.
    std::string failed_version_;
    std::string failure_;

    // Empty when there is no user data directory: nothing is remembered.
    std::filesystem::path data_dir_;
    // sdk_install_directory(). Empty when there is none: only located SDKs are listed.
    std::filesystem::path install_dir_;
    LauncherState state_;
    std::vector<ProjectEntry> projects_;
    std::vector<SdkEntry> sdks_;

    Page page_ = Page::Projects;
    // The SDK picked on the New project page, by root because refresh() reorders sdks_. Empty: none.
    std::filesystem::path new_sdk_root_;
    // Why Create is disabled, or nullopt.
    std::optional<std::string> new_project_problem_;

    engine::FileDialogCall dialog_;
    DialogPurpose dialog_purpose_ = DialogPurpose::AddProject;
    LauncherRequest request_;
};

}
