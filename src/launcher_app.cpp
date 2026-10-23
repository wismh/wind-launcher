#include "launcher_app.h"

#include "user_paths.h"

#include <asset_ids.h>

#include <engine/core/platform.h>
#include <engine/core/window_control.h>
#include <engine/ecs/schedule.h>
#include <engine/ecs/world.h>
#include <engine/log.h>
#include <engine/process/process_launcher.h>
#include <engine/ui/canvas.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>
#include <utility>

namespace launcher {
namespace {

constexpr char kOrganization[] = "Wind";
constexpr char kApplication[] = "Launcher";
constexpr char kStateFile[] = "launcher.txt";
constexpr char kReleaseCacheFile[] = "available_sdks.txt";
constexpr char kMissingTone[] = "#ff6b68";

std::string sdk_detail(const SdkEntry& sdk) {
    std::string detail = sdk.manifest.config;
    if (sdk.manifest.dirty) {
        detail += ", local changes";
    }
    detail += ", commit " + sdk.manifest.commit.substr(0, 8);
    detail += sdk.located ? ", located" : ", installed";
    if (has_symbols(sdk)) {
        detail += ", debug symbols";
    }
    return detail;
}

bool has_template(const SdkEntry& sdk) {
    std::error_code error;
    return std::filesystem::is_directory(project_template(sdk), error);
}

std::string error_text(engine::ProcessError error, std::string_view program) {
    switch (error) {
        case engine::ProcessError::NotFound:
            return std::string(program) + " is missing";
        case engine::ProcessError::StartFailed:
            return std::string(program) + " could not start";
        case engine::ProcessError::Unsupported:
            return "starting programs is not supported on this platform";
    }
    return std::string(program) + " could not start";
}

}

LauncherApp::LauncherApp(const engine::EngineServices& services)
    : engine::GameBase(services.worlds)
    , windows_(&services.windows)
    , processes_(&services.processes)
    , view_model_(std::make_shared<LauncherViewModel>())
    , feed_downloader_(services.processes)
    , install_downloader_(services.processes)
    , extractor_(services.processes)
    , installer_(install_downloader_, extractor_) {
    view_model_->showProjects.bind_to<LauncherApp, &LauncherApp::show_projects>(*this);
    view_model_->showSdks.bind_to<LauncherApp, &LauncherApp::show_sdks>(*this);
    view_model_->installSdk.bind_to<LauncherApp, &LauncherApp::show_install_sdk>(*this);
    view_model_->closeInstallSdk.bind_to<LauncherApp, &LauncherApp::show_sdks>(*this);
    view_model_->closeInstallOptions.bind_to<LauncherApp, &LauncherApp::close_install_options>(*this);
    view_model_->confirmInstall.bind_to<LauncherApp, &LauncherApp::confirm_install>(*this);
    view_model_->addProject.bind_to<LauncherApp, &LauncherApp::add_project>(*this);
    view_model_->locateSdk.bind_to<LauncherApp, &LauncherApp::locate_sdk>(*this);
    view_model_->refreshAvailable.bind_to<LauncherApp, &LauncherApp::refresh_available>(*this);
    view_model_->showSdkFolder.bind_to<LauncherApp, &LauncherApp::show_sdk_folder>(*this);
    view_model_->newProject.bind_to<LauncherApp, &LauncherApp::new_project>(*this);
    view_model_->browseLocation.bind_to<LauncherApp, &LauncherApp::browse_location>(*this);
    view_model_->toggleSdkPicker.bind_to<LauncherApp, &LauncherApp::toggle_sdk_picker>(*this);
    view_model_->createProject.bind_to<LauncherApp, &LauncherApp::create_project, &LauncherApp::can_create_project>(
            *this);
    view_model_->cancelNewProject.bind_to<LauncherApp, &LauncherApp::cancel_new_project>(*this);
}

engine::WindowDesc LauncherApp::primary_window() const {
    return engine::WindowDesc{.title = "Wind Launcher", .size = {960, 620}};
}

void LauncherApp::on_start() {
    engine::ecs::World& ui = world();
    const engine::ecs::Entity canvas = ui.create();
    ui.emplace<engine::ui::UiCanvas>(canvas, engine::ui::UiCanvas{
            .document = assets::ui::launcher,
            .stylesheet = assets::css::launcher,
            .data_context = view_model_,
            .fit = engine::ui::UiFit::FillWindow,
    });
    ui.add_system(engine::ecs::Schedule::Frame, engine::ecs::Phase::Game, [this](engine::ecs::World&) { frame(); });

    if (auto directory = engine::user_data_directory(kOrganization, kApplication)) {
        data_dir_ = std::move(*directory);
        state_ = load_launcher_state(data_dir_ / kStateFile);
    } else {
        engine::log::warn("Launcher: no user data directory (" + directory.error().message() +
                "). Nothing is remembered");
    }
    install_dir_ = sdk_install_directory();
    if (install_dir_.empty()) {
        engine::log::warn("Launcher: no SDK install directory. Only located SDKs are listed");
        view_model_->sdkFolderText = std::string("No install directory: locate an SDK by its sdk.toml.");
    } else {
        remove_deleted_sdks(install_dir_);
        remove_interrupted_installs(install_dir_);
        view_model_->sdkFolderText = "Installed in " + path_text(install_dir_);
    }

    ReleaseFeedOptions feed_options;
    feed_options.cache_file = data_dir_.empty() ? std::filesystem::path{} : data_dir_ / kReleaseCacheFile;
    feed_options.work_directory = (data_dir_.empty() ? std::filesystem::temp_directory_path() / "wind-launcher"
                                                     : data_dir_) / "downloads";
    for (const char* variable : {"GITHUB_TOKEN", "GH_TOKEN"}) {
        if (feed_options.token.empty()) {
            feed_options.token = environment_path(variable).string();
        }
    }
    feed_ = std::make_unique<ReleaseFeed>(feed_downloader_, std::move(feed_options));
    feed_->load_cache();
    refresh();
    feed_->refresh();
    update_available_note();
    if (projects_.empty()) {
        show_status("Make a project with New project, or add one you have with Add existing.");
    }
}

void LauncherApp::show_projects() {
    show_page(Page::Projects);
}

void LauncherApp::show_sdks() {
    show_page(Page::Sdks);
}

void LauncherApp::show_install_sdk() {
    show_page(Page::InstallSdk);
}

void LauncherApp::close_install_options() {
    show_page(Page::InstallSdk);
}

void LauncherApp::confirm_install() {
    request(LauncherRequest{.kind = LauncherRequest::Kind::ConfirmInstall});
}

void LauncherApp::show_page(Page page) {
    page_ = page;
    const auto display = [&](Page shown) { return std::string(page == shown ? "block" : "none"); };
    const bool sdk_page = page == Page::Sdks || page == Page::InstallSdk || page == Page::InstallOptions;
    view_model_->projectsTab = !sdk_page;
    view_model_->sdksTab = sdk_page;
    view_model_->projectsDisplay = display(Page::Projects);
    view_model_->sdksDisplay = display(Page::Sdks);
    view_model_->availableDisplay = display(Page::InstallSdk);
    view_model_->optionsDisplay = display(Page::InstallOptions);
    view_model_->newProjectDisplay = display(Page::NewProject);
    view_model_->sdkPickerOpen = false;
}

void LauncherApp::add_project() {
    request(LauncherRequest{.kind = LauncherRequest::Kind::AddProject});
}

void LauncherApp::new_project() {
    std::filesystem::path location = state_.location.empty() ? default_project_location() : state_.location;
    view_model_->newLocation = location.empty() ? std::string{} : path_text(location);
    // "My Game", or "My Game 2" and on when that directory is taken.
    std::string name = "My Game";
    for (int n = 2; n < 100 && new_project_problem(NewProject{.name = name, .location = location}); ++n) {
        name = "My Game " + std::to_string(n);
    }
    view_model_->newName = name;
    new_sdk_root_.clear();
    refresh_sdk_options();
    show_page(Page::NewProject);
    check_new_project();
}

void LauncherApp::browse_location() {
    request(LauncherRequest{.kind = LauncherRequest::Kind::BrowseLocation});
}

void LauncherApp::toggle_sdk_picker() {
    view_model_->sdkPickerOpen = !view_model_->sdkPickerOpen.get();
}

void LauncherApp::pick_new_project_sdk(std::size_t index) {
    view_model_->sdkPickerOpen = false;
    if (index < sdks_.size()) {
        new_sdk_root_ = sdks_[index].root;
        refresh_sdk_options();
        check_new_project();
    }
}

void LauncherApp::create_project() {
    request(LauncherRequest{.kind = LauncherRequest::Kind::CreateProject});
}

bool LauncherApp::can_create_project() const {
    return page_ == Page::NewProject && !new_project_problem_;
}

void LauncherApp::cancel_new_project() {
    show_page(Page::Projects);
}

void LauncherApp::locate_sdk() {
    request(LauncherRequest{.kind = LauncherRequest::Kind::LocateSdk});
}

void LauncherApp::show_sdk_folder() {
    request(LauncherRequest{.kind = LauncherRequest::Kind::ShowSdkFolder});
}

void LauncherApp::refresh_available() {
    request(LauncherRequest{.kind = LauncherRequest::Kind::RefreshAvailable});
}

void LauncherApp::request(LauncherRequest request) {
    request_ = std::move(request);
}

void LauncherApp::frame() {
    if (const std::optional<engine::FileDialogResult> answer = dialog_.take()) {
        take_dialog_answer(*answer);
    }
    feed_->poll();
    installer_.poll();
    if (const std::optional<InstallOutcome> outcome = installer_.take_outcome()) {
        finish_install(*outcome);
    }
    if (feed_->revision() != feed_revision_) {
        feed_revision_ = feed_->revision();
        rebuild_available();
        rebuild_project_rows();
        rebuild_sdk_rows();
        update_available_note();
    }
    act(std::exchange(request_, LauncherRequest{}));
    show_install_progress();
    if (page_ == Page::NewProject) {
        check_new_project();
    }
}

void LauncherApp::act(const LauncherRequest& request) {
    switch (request.kind) {
        case LauncherRequest::Kind::None:
            break;
        case LauncherRequest::Kind::AddProject:
            open_dialog(DialogPurpose::AddProject);
            break;
        case LauncherRequest::Kind::LocateSdk:
            open_dialog(DialogPurpose::LocateSdk);
            break;
        case LauncherRequest::Kind::ShowSdkFolder:
            if (!install_dir_.empty()) {
                std::error_code error;
                std::filesystem::create_directories(install_dir_, error);
                show_folder(install_dir_);
            }
            break;
        case LauncherRequest::Kind::BrowseLocation:
            open_dialog(DialogPurpose::ProjectLocation);
            break;
        case LauncherRequest::Kind::CreateProject:
            make_project();
            break;
        case LauncherRequest::Kind::Open:
            open_project(request.index);
            break;
        case LauncherRequest::Kind::Remove:
            if (request.index < projects_.size()) {
                forget_project(state_, projects_[request.index].directory);
                refresh();
                show_status("Removed from the list. The project's files are untouched.");
            }
            break;
        case LauncherRequest::Kind::ChangeProjectSdk:
            change_project_sdk(request.index, request.version);
            break;
        case LauncherRequest::Kind::ShowSdk:
            if (request.index < sdks_.size()) {
                show_folder(sdks_[request.index].root);
            }
            break;
        case LauncherRequest::Kind::ForgetSdk:
            if (request.index < sdks_.size() && sdks_[request.index].located) {
                forget_sdk(state_, sdks_[request.index].root);
                refresh();
                show_status("Forgot the SDK. Its files are untouched.");
            }
            break;
        case LauncherRequest::Kind::DeleteSdk:
            delete_sdk(request.index);
            break;
        case LauncherRequest::Kind::InstallSdk:
            show_install_options(request.index);
            break;
        case LauncherRequest::Kind::ConfirmInstall:
            if (const AvailableSdk* sdk = find_available(options_version_)) {
                const bool symbols = sdk->release.symbols && view_model_->symbolsChecked.get();
                start_install(sdk->release, InstallOptions{.symbols = symbols});
            }
            show_page(Page::InstallSdk);
            break;
        case LauncherRequest::Kind::AddSymbols:
            add_symbols(request.index);
            break;
        case LauncherRequest::Kind::InstallVersion:
            if (const AvailableSdk* sdk = find_available(request.version)) {
                start_install(sdk->release);
            }
            break;
        case LauncherRequest::Kind::CancelInstall:
            installer_.cancel();
            if (const std::optional<InstallOutcome> outcome = installer_.take_outcome()) {
                finish_install(*outcome);
            }
            break;
        case LauncherRequest::Kind::RefreshAvailable:
            feed_->refresh();
            update_available_note();
            break;
    }
}

void LauncherApp::open_dialog(DialogPurpose purpose) {
    if (dialog_.pending()) {
        return;
    }
    dialog_purpose_ = purpose;
    if (purpose == DialogPurpose::ProjectLocation) {
        dialog_ = windows_->request_open_folder(
                engine::kPrimaryWindow, path_from_text(view_model_->newLocation.get()));
        return;
    }
    std::vector<engine::FileFilter> filters{
            purpose == DialogPurpose::AddProject
                    ? engine::FileFilter{.name = "Wind project (wind_project.toml)", .pattern = "toml"}
                    : engine::FileFilter{.name = "Wind editor SDK (sdk.toml)", .pattern = "toml"},
    };
    dialog_ = windows_->request_open_file(engine::kPrimaryWindow, std::move(filters));
}

void LauncherApp::take_dialog_answer(const engine::FileDialogResult& answer) {
    if (!answer.path) {
        return;
    }
    const std::filesystem::path& file = *answer.path;
    if (dialog_purpose_ == DialogPurpose::ProjectLocation) {
        view_model_->newLocation = path_text(file);
        return;
    }
    if (dialog_purpose_ == DialogPurpose::AddProject) {
        if (file.filename() != engine::kWindProjectFile) {
            show_status("Pick the project's wind_project.toml.");
            return;
        }
        remember_project(state_, file.parent_path());
        refresh();
        show_status(projects_.front().project ? "Added " + projects_.front().project->name + "."
                                              : projects_.front().problem);
        return;
    }
    if (file.filename() != engine::kSdkManifestFile) {
        show_status("Pick the SDK's sdk.toml (beside its bin/).");
        return;
    }
    const auto manifest = engine::read_sdk_manifest(file.parent_path());
    if (!manifest) {
        show_status(engine::describe(manifest.error()));
        return;
    }
    remember_sdk(state_, file.parent_path());
    refresh();
    show_status("Located SDK " + manifest->version + ".");
}

void LauncherApp::open_project(std::size_t index, const SdkEntry* in_sdk) {
    if (index >= projects_.size() || !projects_[index].project) {
        return;
    }
    const ProjectEntry entry = projects_[index];
    const SdkEntry* sdk = in_sdk != nullptr ? in_sdk : sdk_for(sdks_, entry.project->engine);
    if (sdk == nullptr) {
        show_status(entry.project->name + " needs SDK " + entry.project->engine + ", which is not installed.");
        return;
    }
    const std::expected<void, engine::ProcessError> launched = processes_->launch(editor_launch(*sdk, entry.directory));
    if (!launched) {
        show_status("Could not open " + entry.project->name + ": " + error_text(launched.error(), "the editor") +
                " (" + path_text(editor_executable(*sdk)) + ").");
        return;
    }
    // refresh() rebuilds sdks_, which `sdk` points into: keep what the status line needs first.
    const std::string version = sdk->manifest.version;
    engine::log::info("Launcher: opened " + path_text(entry.directory) + " in editor " + version + " at " +
            path_text(sdk->root));
    remember_project(state_, entry.directory);
    refresh();
    show_status("Opened " + entry.project->name + " in editor " + version + ".");
}

void LauncherApp::change_project_sdk(std::size_t index, const std::string& version) {
    if (index >= projects_.size() || !projects_[index].project || projects_[index].project->engine == version) {
        return;
    }
    const ProjectEntry entry = projects_[index];
    const std::expected<void, std::string> written = set_project_engine(entry.directory, version);
    engine::log::info("Launcher: SDK of " + path_text(entry.directory) + " from " + entry.project->engine + " to " +
            version + ": " + (written ? std::string("done") : written.error()));
    refresh();
    show_status(written ? entry.project->name + " now uses SDK " + version + "." : written.error());
}

void LauncherApp::delete_sdk(std::size_t index) {
    // Only an SDK found under the install directory: a located one is the user's own build, which Forget leaves be.
    if (index >= sdks_.size() || sdks_[index].located) {
        return;
    }
    const SdkEntry sdk = sdks_[index];
    const std::expected<void, std::string> deleted = launcher::delete_sdk(sdk.root);
    engine::log::info("Launcher: delete SDK " + sdk.manifest.version + " at " + path_text(sdk.root) + ": " +
            (deleted ? std::string("done") : deleted.error()));
    refresh();
    show_status(deleted ? "Deleted SDK " + sdk.manifest.version + "." : deleted.error());
}

void LauncherApp::show_folder(const std::filesystem::path& directory) {
    const std::expected<void, engine::ProcessError> launched = processes_->launch(folder_launch(directory));
    if (!launched) {
        show_status("Could not show " + path_text(directory) + ": " +
                error_text(launched.error(), "the file manager") + ".");
    }
}

const SdkEntry* LauncherApp::picked_sdk() const {
    const auto sdk = std::ranges::find_if(sdks_, [&](const SdkEntry& entry) { return entry.root == new_sdk_root_; });
    return sdk == sdks_.end() ? nullptr : &*sdk;
}

NewProject LauncherApp::new_project_form() const {
    NewProject project{
            .name = view_model_->newName.get(),
            .location = path_from_text(view_model_->newLocation.get()),
    };
    if (const SdkEntry* sdk = picked_sdk()) {
        project.sdk_root = sdk->root;
        project.engine = sdk->manifest.version;
    }
    return project;
}

void LauncherApp::check_new_project() {
    const NewProject project = new_project_form();
    const SdkEntry* sdk = picked_sdk();
    new_project_problem_ = new_project_problem(project);
    if (!new_project_problem_ && sdk == nullptr) {
        new_project_problem_ = "Install or locate an SDK first (SDKs page).";
    } else if (!new_project_problem_ && !has_template(*sdk)) {
        new_project_problem_ = "SDK " + project.engine +
                               " has no project template. Install an SDK built from a newer engine.";
    }
    if (new_project_problem_) {
        view_model_->newHint = *new_project_problem_;
        view_model_->newHintTone = std::string(kMissingTone);
    } else {
        view_model_->newHint = "Creates " + path_text(project_directory(project)) + ", build target " +
                               project_target(project.name) + ", and opens it in the editor.";
        view_model_->newHintTone = std::string{};
    }
}

void LauncherApp::make_project() {
    check_new_project();
    if (page_ != Page::NewProject || new_project_problem_) {
        return;
    }
    const NewProject project = new_project_form();
    const std::expected<std::filesystem::path, std::string> created =
            launcher::create_project(project_template(*picked_sdk()), project);
    if (!created) {
        show_status(created.error());
        return;
    }
    engine::log::info("Launcher: created " + path_text(*created) + " from SDK " + project.engine);
    state_.location = project.location;
    remember_project(state_, *created);
    refresh();
    show_page(Page::Projects);
    // In the SDK picked, not whichever of that version Open would prefer: a dirty dev SDK loses to a clean one there.
    open_project(0, picked_sdk());
}

void LauncherApp::refresh_sdk_options() {
    if (picked_sdk() == nullptr) {
        // The newest SDK that can make projects, else the newest.
        const auto usable = std::ranges::find_if(sdks_, has_template);
        new_sdk_root_ = usable != sdks_.end() ? usable->root : sdks_.empty() ? std::filesystem::path{} : sdks_[0].root;
    }

    std::vector<std::shared_ptr<SdkOptionViewModel>> options;
    view_model_->newSdkText = std::string("No SDK");
    for (std::size_t i = 0; i < sdks_.size(); ++i) {
        auto option = std::make_shared<SdkOptionViewModel>(*this, i);
        std::string text = sdks_[i].manifest.version + "   " + sdk_detail(sdks_[i]);
        if (!has_template(sdks_[i])) {
            text += ", no template";
        }
        option->selected = sdks_[i].root == new_sdk_root_;
        if (option->selected.get()) {
            view_model_->newSdkText = text;
        }
        option->text = std::move(text);
        options.push_back(std::move(option));
    }
    view_model_->sdkOptions.set(std::move(options));
}

void LauncherApp::rebuild_project_rows() {
    std::vector<std::shared_ptr<ProjectRowViewModel>> project_rows;
    for (std::size_t i = 0; i < projects_.size(); ++i) {
        const ProjectEntry& entry = projects_[i];
        const SdkEntry* sdk = entry.project ? sdk_for(sdks_, entry.project->engine) : nullptr;
        auto row = std::make_shared<ProjectRowViewModel>(*this, i, sdk != nullptr);
        row->path = path_text(entry.directory);
        if (entry.project) {
            std::vector<std::shared_ptr<ProjectSdkChoiceViewModel>> choices;
            for (const SdkEntry& installed : sdks_) {
                const std::string& version = installed.manifest.version;
                auto choice = std::make_shared<ProjectSdkChoiceViewModel>(*this, i, version);
                choice->text = installed.located ? version + "  (located)" : version;
                choice->selected = version == entry.project->engine;
                choices.push_back(std::move(choice));
            }
            row->set_sdk_choices(std::move(choices));
        }
        if (entry.project && sdk == nullptr) {
            // The SDK is not here, but GitHub has it: Install in the place of Open.
            if (const AvailableSdk* offered = find_available(entry.project->engine); offered != nullptr && !offered->installed) {
                const bool installing_it = installer_.running() && installer_.progress().version == offered->release.version;
                row->offer_install(offered->release.version,
                        installing_it ? std::string("Installing…") : "Install " + offered->release.version,
                        !installer_.running());
            }
        }
        if (entry.project) {
            row->name = entry.project->name;
            row->engineText = (sdk != nullptr ? "SDK " : "Needs SDK ") + entry.project->engine;
            row->tone = std::string(sdk != nullptr ? "" : kMissingTone);
        } else {
            row->name = path_text(entry.directory.filename());
            row->engineText = std::string("Cannot read");
            row->tone = std::string(kMissingTone);
        }
        project_rows.push_back(std::move(row));
    }
    view_model_->projects.set(std::move(project_rows));
}

void LauncherApp::rebuild_sdk_rows() {
    std::vector<std::shared_ptr<SdkRowViewModel>> sdk_rows;
    for (std::size_t i = 0; i < sdks_.size(); ++i) {
        auto row = std::make_shared<SdkRowViewModel>(*this, i, sdks_[i].located);
        row->version = sdks_[i].manifest.version;
        row->detail = sdk_detail(sdks_[i]);
        row->path = path_text(sdks_[i].root);
        // Only an SDK under the install directory: a located one is the user's own build.
        const AvailableSdk* offered = sdks_[i].located ? nullptr : find_available(sdks_[i].manifest.version);
        if (offered != nullptr && offered->release.symbols && !has_symbols(sdks_[i]) && !installer_.running()) {
            row->canAddSymbols = true;
            row->symbolsLabel = "Add debug symbols (" + format_size(offered->release.symbols->size) + ")";
        }
        sdk_rows.push_back(std::move(row));
    }
    view_model_->sdks.set(std::move(sdk_rows));
}

void LauncherApp::refresh() {
    std::vector<std::string> problems;
    sdks_ = find_sdks(install_dir_, state_.sdks, problems);
    for (const std::string& problem : problems) {
        engine::log::warn("Launcher: " + problem);
    }
    projects_.clear();
    for (const std::filesystem::path& directory : state_.projects) {
        projects_.push_back(read_project_entry(directory));
    }

    rebuild_available();
    rebuild_project_rows();

    rebuild_sdk_rows();
    refresh_sdk_options();

    if (!data_dir_.empty() && !save_launcher_state(data_dir_ / kStateFile, state_)) {
        engine::log::warn("Launcher: could not save " + path_text(data_dir_ / kStateFile));
    }
}

const AvailableSdk* LauncherApp::find_available(std::string_view version) const {
    const auto found = std::ranges::find_if(
            available_, [&](const AvailableSdk& sdk) { return sdk.release.version == version; });
    return found == available_.end() ? nullptr : &*found;
}

void LauncherApp::show_install_options(std::size_t index) {
    if (index >= available_.size()) {
        return;
    }
    const SdkRelease& release = available_[index].release;
    options_version_ = release.version;
    view_model_->optionsTitle = "Install SDK " + release.version;
    view_model_->optionsSdkDetail = release.size > 0 ? format_size(release.size) : std::string{};
    view_model_->symbolsAvailable = release.symbols.has_value();
    view_model_->symbolsMissing = !release.symbols.has_value();
    view_model_->symbolsChecked = false;
    view_model_->symbolsDetail = release.symbols ? format_size(release.symbols->size) + ", for debugging the engine"
                                                 : std::string{};
    show_page(Page::InstallOptions);
}

void LauncherApp::start_install(const SdkRelease& release, InstallOptions options) {
    // `release` points into available_, which rebuild_available() replaces below.
    const SdkRelease chosen = release;
    failed_version_.clear();
    failure_.clear();
    const std::expected<void, std::string> started = installer_.start(chosen, install_dir_, options);
    if (!started) {
        show_status(started.error());
        return;
    }
    engine::log::info("Launcher: installing SDK " + chosen.version + " from " + chosen.archive_url +
            (options.symbols ? " with debug symbols" : ""));
    show_status("Installing SDK " + chosen.version + "…");
    rebuild_available();
    rebuild_project_rows();
    rebuild_sdk_rows();
}

void LauncherApp::add_symbols(std::size_t index) {
    if (index >= sdks_.size() || sdks_[index].located) {
        return;
    }
    const AvailableSdk* offered = find_available(sdks_[index].manifest.version);
    if (offered == nullptr) {
        show_status("GitHub has no release of SDK " + sdks_[index].manifest.version + " to take its symbols from.");
        return;
    }
    failed_version_.clear();
    failure_.clear();
    const std::expected<void, std::string> started = installer_.add_symbols(offered->release, sdks_[index].root);
    if (!started) {
        show_status(started.error());
        return;
    }
    engine::log::info("Launcher: adding the debug symbols of SDK " + offered->release.version + " from " +
            offered->release.symbols->url);
    show_status("Adding the debug symbols of SDK " + offered->release.version + "…");
    rebuild_available();
    rebuild_project_rows();
    rebuild_sdk_rows();
}

void LauncherApp::finish_install(const InstallOutcome& outcome) {
    engine::log::info("Launcher: install of SDK " + outcome.version + ": " + outcome.message);
    failed_version_.clear();
    failure_.clear();
    if (!outcome.installed && !outcome.cancelled) {
        failed_version_ = outcome.version;
        failure_ = outcome.message;
    }
    if (outcome.installed) {
        // The new SDK is in the Installed list at once, and a project that waited for it can be opened.
        refresh();
    } else {
        rebuild_available();
        rebuild_project_rows();
        rebuild_sdk_rows();
    }
    view_model_->installText = std::string{};
    view_model_->statusLimit = std::string("100%");
    show_status(outcome.message);
}

void LauncherApp::rebuild_available() {
    available_ = build_available(feed_->releases(), sdks_);
    available_rows_.clear();
    const bool busy = installer_.running();
    for (std::size_t i = 0; i < available_.size(); ++i) {
        const AvailableSdk& sdk = available_[i];
        const bool installing = busy && installer_.progress().version == sdk.release.version;
        auto row = std::make_shared<AvailableSdkRowViewModel>(*this, i, !busy && !sdk.installed);
        row->version = sdk.release.version;
        std::string detail(stage_name(sdk.release.parsed));
        if (!sdk.release.date.empty()) {
            detail += ", " + sdk.release.date;
        }
        if (sdk.release.size > 0) {
            detail += ", " + format_size(sdk.release.size);
        }
        if (sdk.newest) {
            detail += ", newest";
        }
        row->detail = std::move(detail);
        if (sdk.installed) {
            row->state = std::string("Installed");
            row->installDisplay = std::string("none");
        } else if (installing) {
            row->installDisplay = std::string("none");
            row->cancelDisplay = std::string("block");
        } else if (failed_version_ == sdk.release.version) {
            row->state = failure_;
            row->tone = std::string(kMissingTone);
        }
        available_rows_.push_back(std::move(row));
    }
    view_model_->available.set(available_rows_);
    show_install_progress();
}

namespace {

std::string percent(std::uint64_t done, std::uint64_t total) {
    return total == 0 ? std::string{} : " (" + std::to_string(std::min<std::uint64_t>(100, done * 100 / total)) + "%)";
}

}

void LauncherApp::show_install_progress() {
    if (!installer_.running()) {
        return;
    }
    const InstallProgress& progress = installer_.progress();
    std::string text;
    switch (progress.phase) {
        case InstallPhase::Idle:
            return;
        case InstallPhase::Checksum:
            text = "Getting the checksum…";
            break;
        case InstallPhase::Download:
            text = "Downloading " + format_size(progress.done) +
                   (progress.total > 0 ? " of " + format_size(progress.total) : std::string{}) +
                   percent(progress.done, progress.total);
            break;
        case InstallPhase::Verify:
            text = "Checking the download" + percent(progress.done, progress.total);
            break;
        case InstallPhase::Unpack:
            text = "Unpacking…";
            break;
    }
    if (progress.symbols) {
        text = "Debug symbols: " + text;
    }
    view_model_->installText = "SDK " + progress.version + ": " + text;
    view_model_->statusLimit = std::string("calc(100% - 376px)");
    for (std::size_t i = 0; i < available_.size() && i < available_rows_.size(); ++i) {
        if (available_[i].release.version == progress.version) {
            available_rows_[i]->state = text;
        }
    }
}

void LauncherApp::update_available_note() {
    if (feed_->loading()) {
        view_model_->availableNote = std::string("Checking GitHub…");
    } else if (!feed_->note().empty()) {
        view_model_->availableNote = feed_->note();
    } else if (available_.empty()) {
        const Platform platform = current_platform();
        view_model_->availableNote = "No SDK for " + platform.os + "-" + platform.arch + " is published yet.";
    } else {
        view_model_->availableNote = std::string{};
    }
}

void LauncherApp::show_status(std::string text) {
    view_model_->statusText = std::move(text);
}

}
