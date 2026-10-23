#include <gtest/gtest.h>

#include "launcher_state.h"
#include "project_entry.h"
#include "project_template.h"
#include "sdk_catalog.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using launcher_test::TempDir;

TEST(LauncherState, ParsesProjectsAndSdksAndSkipsTheRest) {
    const launcher::LauncherState state = launcher::parse_launcher_state(
            "# comment\r\nproject=C:/games/ttt\r\nsdk=C:/engine/out/sdk\nwindow=10,10\n\nproject=C:/games/emf\nproject=\n");
    ASSERT_EQ(state.projects.size(), 2u);
    EXPECT_EQ(state.projects[0], std::filesystem::path("C:/games/ttt"));
    EXPECT_EQ(state.projects[1], std::filesystem::path("C:/games/emf"));
    ASSERT_EQ(state.sdks.size(), 1u);
    EXPECT_EQ(state.sdks[0], std::filesystem::path("C:/engine/out/sdk"));
}

TEST(LauncherState, FormatsWhatItParses) {
    launcher::LauncherState state;
    state.projects = {std::filesystem::path(u8"C:/ігри/ttt"), "D:/emf"};
    state.sdks = {"C:/engine/out/sdk"};
    const launcher::LauncherState again = launcher::parse_launcher_state(launcher::format_launcher_state(state));
    EXPECT_EQ(again.projects, state.projects);
    EXPECT_EQ(again.sdks, state.sdks);
}

TEST(LauncherState, SavesAndLoadsAndAMissingFileIsEmpty) {
    const TempDir dir;
    EXPECT_TRUE(launcher::load_launcher_state(dir.path() / "launcher.txt").projects.empty());
    launcher::LauncherState state;
    state.projects = {"C:/games/ttt"};
    ASSERT_TRUE(launcher::save_launcher_state(dir.path() / "launcher.txt", state));
    EXPECT_EQ(launcher::load_launcher_state(dir.path() / "launcher.txt").projects, state.projects);
}

TEST(LauncherState, RememberPutsTheProjectFirstOnce) {
    launcher::LauncherState state;
    launcher::remember_project(state, "C:/games/a");
    launcher::remember_project(state, "C:/games/b");
    launcher::remember_project(state, "C:/games/a/");
    ASSERT_EQ(state.projects.size(), 2u);
    EXPECT_TRUE(launcher::same_directory(state.projects[0], "C:/games/a"));
    EXPECT_TRUE(launcher::same_directory(state.projects[1], "C:/games/b"));
    launcher::forget_project(state, "C:/games/b");
    EXPECT_EQ(state.projects.size(), 1u);
#if defined(_WIN32)
    EXPECT_TRUE(launcher::same_directory("C:/Games/A", "c:\\games\\a"));
#endif
}

TEST(LauncherState, RememberSdkKeepsOrderWithoutDuplicates) {
    launcher::LauncherState state;
    launcher::remember_sdk(state, "C:/sdk/one");
    launcher::remember_sdk(state, "C:/sdk/two");
    launcher::remember_sdk(state, "C:/sdk/one/");
    ASSERT_EQ(state.sdks.size(), 2u);
    EXPECT_EQ(state.sdks[0], std::filesystem::path("C:/sdk/one"));
    launcher::forget_sdk(state, "C:/sdk/one");
    ASSERT_EQ(state.sdks.size(), 1u);
    EXPECT_EQ(state.sdks[0], std::filesystem::path("C:/sdk/two"));
}

TEST(SdkCatalog, ComparesVersionsByNumber) {
    EXPECT_GT(launcher::compare_versions("0.10.0", "0.9.1"), 0);
    EXPECT_LT(launcher::compare_versions("0.1.0", "0.1.1"), 0);
    EXPECT_EQ(launcher::compare_versions("1.2.3", "1.2.3"), 0);
    EXPECT_GT(launcher::compare_versions("10.0.0", "2.0.0"), 0);
    EXPECT_GT(launcher::compare_versions("0.1.100a", "0.1.99a"), 0);
    EXPECT_GT(launcher::compare_versions("0.2.0a", "0.1.100a"), 0);
    EXPECT_GT(launcher::compare_versions("0.2.6b", "0.2.5a"), 0);
    EXPECT_GT(launcher::compare_versions("0.2.0", "0.2.0b"), 0);
    EXPECT_GT(launcher::compare_versions("0.2.0b", "0.2.0a"), 0);
    EXPECT_LT(launcher::compare_versions("0.2.0a-dev", "0.2.0a"), 0);
    EXPECT_GT(launcher::compare_versions("0.1.0", "nightly"), 0) << "a text that is not a version is older";
    EXPECT_GT(launcher::compare_versions("1.0.0-rc", "1.0.0-beta"), 0) << "two of them compare as text";
}

TEST(SdkCatalog, ListsReleasesNewestFirstAndUnreadableVersionsLast) {
    const TempDir dir;
    for (const std::string_view version :
            {"0.1.99a", "nightly", "0.2.0a-dev", "0.1.100a", "0.2.0b", "0.2.0a", "0.2.0"}) {
        dir.sdk(std::filesystem::path("sdks") / std::string(version), version, false);
    }
    std::vector<std::string> problems;
    const std::vector<launcher::SdkEntry> sdks = launcher::find_sdks(dir.path() / "sdks", {}, problems);
    std::vector<std::string> versions;
    for (const launcher::SdkEntry& sdk : sdks) {
        versions.push_back(sdk.manifest.version);
    }
    const std::vector<std::string> expected{"0.2.0", "0.2.0b", "0.2.0a", "0.2.0a-dev", "0.1.100a", "0.1.99a", "nightly"};
    EXPECT_EQ(versions, expected);
    EXPECT_TRUE(problems.empty());
    ASSERT_NE(launcher::sdk_for(sdks, "0.1.100a"), nullptr);
    EXPECT_EQ(launcher::sdk_for(sdks, "0.1.100"), nullptr) << "a project's version is matched as the exact text";
}

TEST(SdkCatalog, FindsInstalledAndLocatedSdksNewestFirstCleanBeforeDirty) {
    const TempDir dir;
    dir.sdk("sdks/0.1.0-dev", "0.1.0", true);
    dir.sdk("sdks/0.2.0", "0.2.0", false);
    dir.write("sdks/notes/readme.txt", "not an sdk");
    dir.sdk("dev/sdk", "0.1.0", false);
    dir.write("broken/sdk.toml", "version = \n");

    std::vector<std::string> problems;
    const std::vector<launcher::SdkEntry> sdks = launcher::find_sdks(dir.path() / "sdks",
            {dir.path() / "dev" / "sdk", dir.path() / "broken", dir.path() / "sdks" / "0.2.0"}, problems);
    ASSERT_EQ(sdks.size(), 3u);
    EXPECT_EQ(sdks[0].manifest.version, "0.2.0");
    EXPECT_FALSE(sdks[0].located) << "a located root that is also installed is listed once, as installed";
    EXPECT_EQ(sdks[1].manifest.version, "0.1.0");
    EXPECT_FALSE(sdks[1].manifest.dirty);
    EXPECT_TRUE(sdks[1].located);
    EXPECT_TRUE(sdks[2].manifest.dirty);
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_NE(problems[0].find("broken"), std::string::npos);

    const launcher::SdkEntry* match = launcher::sdk_for(sdks, "0.1.0");
    ASSERT_NE(match, nullptr);
    EXPECT_FALSE(match->manifest.dirty);
    EXPECT_EQ(launcher::sdk_for(sdks, "0.3.0"), nullptr);
}

TEST(SdkCatalog, NoInstallDirectoryFindsOnlyLocatedSdks) {
    const TempDir dir;
    dir.sdk("dev", "0.1.0", false);
    std::vector<std::string> problems;
    const auto sdks = launcher::find_sdks({}, {dir.path() / "dev"}, problems);
    ASSERT_EQ(sdks.size(), 1u);
    EXPECT_TRUE(problems.empty());
}

TEST(SdkCatalog, ReadsTheSymbolsMarker) {
    const TempDir dir;
    EXPECT_FALSE(launcher::read_symbols_marker(dir.path() / "none").has_value());
    dir.write("a/symbols.toml",
            "# Wind SDK symbols. Unpack over the SDK of the same version and build.\n"
            "version = \"0.2.9a\"\nbuild_id = \"7e02eea2f11a6923\"\nplatform = \"windows\"\n");
    const auto marker = launcher::read_symbols_marker(dir.path() / "a");
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->version, "0.2.9a");
    EXPECT_EQ(marker->build_id, "7e02eea2f11a6923");

    dir.write("b/symbols.toml", "version = \"0.2.9a\"\n");
    EXPECT_FALSE(launcher::read_symbols_marker(dir.path() / "b").has_value()) << "no build_id";
    dir.write("c/symbols.toml", "version_x = \"1\"\nversion = \"0.2.9a\"\nbuild_id = \"x\"");
    EXPECT_EQ(launcher::read_symbols_marker(dir.path() / "c")->version, "0.2.9a") << "a look-alike key is skipped";
    dir.write("d/symbols.toml", "[table]\nversion = \"0.2.9a\"\nbuild_id = \"x\"\n");
    EXPECT_FALSE(launcher::read_symbols_marker(dir.path() / "d").has_value()) << "only top-level keys";
}

TEST(SdkCatalog, AnSdkHasItsSymbolsOnlyWhenTheMarkerIsOfItsBuild) {
    const TempDir dir;
    dir.sdk("Sdks/sdk-0.1.2a", "0.1.2a", false);
    std::vector<std::string> problems;
    const auto find = [&] { return launcher::find_sdks(dir.path() / "Sdks", {}, problems).at(0); };
    EXPECT_FALSE(launcher::has_symbols(find())) << "no marker";

    dir.write("Sdks/sdk-0.1.2a/symbols.toml", "version = \"0.1.2a\"\nbuild_id = \"aa\"\n");
    EXPECT_TRUE(launcher::has_symbols(find()));
    dir.write("Sdks/sdk-0.1.2a/symbols.toml", "version = \"0.1.2a\"\nbuild_id = \"bb\"\n");
    EXPECT_FALSE(launcher::has_symbols(find())) << "another build";
    dir.write("Sdks/sdk-0.1.2a/symbols.toml", "version = \"0.1.1a\"\nbuild_id = \"aa\"\n");
    EXPECT_FALSE(launcher::has_symbols(find())) << "another version";
}

TEST(SdkCatalog, TheEditorStartsFromItsBinWithTheProject) {
    const launcher::SdkEntry sdk{.root = "C:/sdk", .manifest = {}, .located = true};
    const engine::ProcessDesc desc = launcher::editor_launch(sdk, "C:/games/ttt");
    EXPECT_EQ(desc.program.filename().stem(), "wind_editor");
    EXPECT_EQ(desc.program.parent_path(), std::filesystem::path("C:/sdk/bin"));
    EXPECT_EQ(desc.working_directory, std::filesystem::path("C:/sdk/bin"));
    EXPECT_EQ(desc.arguments, (std::vector<std::string>{"--project", "C:/games/ttt"}));
}

TEST(ProjectEntry, ReadsTheProjectOrSaysWhatIsWrong) {
    const TempDir dir;
    dir.write("game/wind_project.toml", "name = \"Game\"\nengine = \"0.1.0\"\ntarget = \"game\"\n");
    const launcher::ProjectEntry good = launcher::read_project_entry(dir.path() / "game");
    ASSERT_TRUE(good.project.has_value());
    EXPECT_EQ(good.project->name, "Game");
    EXPECT_TRUE(good.problem.empty());

    const launcher::ProjectEntry gone = launcher::read_project_entry(dir.path() / "moved");
    EXPECT_FALSE(gone.project.has_value());
    EXPECT_NE(gone.problem.find("File not found"), std::string::npos);
}

TEST(SdkCatalog, InstallsUnderTheUsersLocalPrograms) {
    const std::filesystem::path dir = launcher::sdk_install_directory();
    ASSERT_FALSE(dir.empty());
    EXPECT_EQ(dir.filename(), "Sdks");
    EXPECT_EQ(dir.parent_path().filename(), "Wind");
#if defined(_WIN32)
    EXPECT_EQ(dir.parent_path().parent_path().filename(), "Programs");
#endif
}

TEST(SdkCatalog, SkipsDirectoriesStartingWithADot) {
    const TempDir dir;
    dir.sdk("sdks/0.1.0", "0.1.0", false);
    dir.sdk("sdks/.deleting-0.2.0", "0.2.0", false);
    std::vector<std::string> problems;
    const auto sdks = launcher::find_sdks(dir.path() / "sdks", {}, problems);
    ASSERT_EQ(sdks.size(), 1u);
    EXPECT_EQ(sdks[0].manifest.version, "0.1.0");
    EXPECT_TRUE(problems.empty());
}

TEST(SdkCatalog, DeleteRemovesTheSdkAndLeavesItsNeighbours) {
    const TempDir dir;
    dir.sdk("sdks/0.1.0", "0.1.0", false);
    dir.write("sdks/0.1.0/bin/wind_editor.exe", "exe");
    dir.sdk("sdks/0.2.0", "0.2.0", false);
    ASSERT_TRUE(launcher::delete_sdk(dir.path() / "sdks" / "0.1.0").has_value());
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "sdks" / "0.1.0"));
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "sdks" / ".deleting-0.1.0"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "sdks" / "0.2.0" / "sdk.toml"));

    const auto gone = launcher::delete_sdk(dir.path() / "sdks" / "0.1.0");
    ASSERT_FALSE(gone.has_value());
    EXPECT_NE(gone.error().find("not there"), std::string::npos);
}

#if defined(_WIN32)
TEST(SdkCatalog, DeleteKeepsAnSdkWhoseFilesAreInUse) {
    const TempDir dir;
    dir.sdk("sdks/0.1.0", "0.1.0", false);
    dir.write("sdks/0.1.0/bin/wind_editor.exe", "exe");
    {
        // An open file without FILE_SHARE_DELETE, like a running editor's executable, blocks renaming its directory.
        std::ifstream running(dir.path() / "sdks" / "0.1.0" / "bin" / "wind_editor.exe");
        ASSERT_TRUE(running.is_open());
        const auto deleted = launcher::delete_sdk(dir.path() / "sdks" / "0.1.0");
        ASSERT_FALSE(deleted.has_value());
        EXPECT_NE(deleted.error().find("Close the editor"), std::string::npos);
    }
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "sdks" / "0.1.0" / "sdk.toml"))
            << "a delete that cannot rename leaves the SDK whole";
}
#endif

TEST(SdkCatalog, RemovesWhatAStoppedDeleteLeft) {
    const TempDir dir;
    dir.sdk("sdks/0.1.0", "0.1.0", false);
    dir.write("sdks/.deleting-0.2.0/bin/wind_editor.exe", "exe");
    launcher::remove_deleted_sdks(dir.path() / "sdks");
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "sdks" / ".deleting-0.2.0"));
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "sdks" / "0.1.0" / "sdk.toml"));
    launcher::remove_deleted_sdks({});
    launcher::remove_deleted_sdks(dir.path() / "missing");
}

TEST(SdkCatalog, TheFileManagerGetsTheNativePath) {
    const engine::ProcessDesc desc = launcher::folder_launch("C:/sdk/0.1.0");
    ASSERT_EQ(desc.arguments.size(), 1u);
#if defined(_WIN32)
    EXPECT_EQ(desc.program, std::filesystem::path("explorer"));
    EXPECT_EQ(desc.arguments[0], R"(C:\sdk\0.1.0)");
#else
    EXPECT_EQ(desc.arguments[0], "C:/sdk/0.1.0");
#endif
}

TEST(LauncherState, RemembersWhereTheLastNewProjectWent) {
    launcher::LauncherState state;
    EXPECT_EQ(launcher::format_launcher_state(state).find("location="), std::string::npos) << "none yet, no line";
    state.location = std::filesystem::path(u8"C:/Users/я/WindProjects");
    const launcher::LauncherState again = launcher::parse_launcher_state(launcher::format_launcher_state(state));
    EXPECT_EQ(again.location, state.location);
}

TEST(ProjectTemplate, TheTargetIsTheNameInLowerCaseWithDashes) {
    EXPECT_EQ(launcher::project_target("My Game"), "my-game");
    EXPECT_EQ(launcher::project_target("  Space -- Shooter 2 "), "space-shooter-2");
    EXPECT_EQ(launcher::project_target("TicTacToe"), "tictactoe");
    EXPECT_EQ(launcher::project_target("2048"), "game-2048");
    EXPECT_EQ(launcher::project_target(reinterpret_cast<const char*>(u8"Гра 1")), "game-1");
    EXPECT_EQ(launcher::project_target(reinterpret_cast<const char*>(u8"Гра")), "");
}

TEST(ProjectTemplate, SaysWhyAProjectCannotBeCreated) {
    const TempDir dir;
    const auto problem = [&](std::string name, std::filesystem::path location) {
        return launcher::new_project_problem(
                launcher::NewProject{.name = std::move(name), .location = std::move(location)});
    };
    EXPECT_FALSE(problem("My Game", dir.path()).has_value());
    EXPECT_FALSE(problem("My Game", dir.path() / "not" / "made" / "yet").has_value()) << "the location is made";
    EXPECT_TRUE(problem("", dir.path()).has_value());
    EXPECT_TRUE(problem("a/b", dir.path()).has_value());
    EXPECT_TRUE(problem("say \"hi\"", dir.path()).has_value());
    EXPECT_TRUE(problem("trailing.", dir.path()).has_value());
    EXPECT_TRUE(problem(" leading", dir.path()).has_value());
    EXPECT_TRUE(problem("CON", dir.path()).has_value());
    EXPECT_TRUE(problem("com1.txt", dir.path()).has_value());
    EXPECT_FALSE(problem("Console", dir.path()).has_value());
    EXPECT_TRUE(problem(reinterpret_cast<const char*>(u8"Гра"), dir.path()).has_value()) << "no target";
    EXPECT_TRUE(problem("My Game", "").has_value());
    EXPECT_TRUE(problem("My Game", "relative/path").has_value());

#if defined(_WIN32)
    const auto deep = problem("My Game", "C:/" + std::string(launcher::kMaxProjectPath, 'd'));
    ASSERT_TRUE(deep.has_value()) << "MSBuild fails under a long path";
    EXPECT_NE(deep->find("path this long"), std::string::npos);
#endif

    std::filesystem::create_directories(dir.path() / "Empty");
    EXPECT_FALSE(problem("Empty", dir.path()).has_value()) << "an empty directory is fine";
    dir.write("Taken/file.txt", "x");
    const auto taken = problem("Taken", dir.path());
    ASSERT_TRUE(taken.has_value());
    EXPECT_NE(taken->find("already exists"), std::string::npos);
}

TEST(ProjectTemplate, CopiesTheTemplateAndFillsItIn) {
    const TempDir dir;
    dir.write("sdk/templates/empty/wind_project.toml",
            "name = \"{{name}}\"\nengine = \"{{engine}}\"\ntarget = \"{{target}}\"\n");
    dir.write("sdk/templates/empty/src/game.cpp", "title = \"{{name}}\"; // {{name}} twice, sdk {{sdk}}\n");
    dir.write("sdk/templates/empty/assets/.gitkeep", "");
    const launcher::NewProject project{
            .name = "My Game",
            .location = dir.path() / "projects",
            .sdk_root = "C:/sdk/0.3.0",
            .engine = "0.3.0",
    };
    const auto created = launcher::create_project(dir.path() / "sdk" / "templates" / "empty", project);
    ASSERT_TRUE(created.has_value()) << created.error();
    EXPECT_EQ(*created, dir.path() / "projects" / "My Game");

    const launcher::ProjectEntry entry = launcher::read_project_entry(*created);
    ASSERT_TRUE(entry.project.has_value()) << entry.problem;
    EXPECT_EQ(entry.project->name, "My Game");
    EXPECT_EQ(entry.project->engine, "0.3.0");
    EXPECT_EQ(entry.project->target, "my-game");

    std::ifstream game(*created / "src" / "game.cpp");
    const std::string text((std::istreambuf_iterator<char>(game)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text, "title = \"My Game\"; // My Game twice, sdk C:/sdk/0.3.0\n");
    EXPECT_TRUE(std::filesystem::exists(*created / "assets" / ".gitkeep"));

    const auto again = launcher::create_project(dir.path() / "sdk" / "templates" / "empty", project);
    ASSERT_FALSE(again.has_value());
    EXPECT_NE(again.error().find("already exists"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(*created / "wind_project.toml")) << "a refused create leaves the project be";
}

TEST(ProjectTemplate, AMissingTemplateMakesNothing) {
    const TempDir dir;
    const launcher::NewProject project{.name = "Game", .location = dir.path(), .sdk_root = {}, .engine = "0.1.0"};
    const auto created = launcher::create_project(dir.path() / "no-template", project);
    ASSERT_FALSE(created.has_value());
    EXPECT_NE(created.error().find("no project template"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "Game"));
}

TEST(ProjectTemplate, TheEnginesOwnTemplateNamesEveryPlaceholder) {
    // templates/empty of the SDK: it makes a project that reads back.
    const std::filesystem::path engine_template = std::filesystem::path(WIND_SDK_TEMPLATES_DIR) / "empty";
    ASSERT_TRUE(std::filesystem::is_directory(engine_template));
    const TempDir dir;
    const launcher::NewProject project{
            .name = "Template Check", .location = dir.path(), .sdk_root = "C:/sdk", .engine = "9.9.9"};
    const auto created = launcher::create_project(engine_template, project);
    ASSERT_TRUE(created.has_value()) << created.error();
    const launcher::ProjectEntry entry = launcher::read_project_entry(*created);
    ASSERT_TRUE(entry.project.has_value()) << entry.problem;
    EXPECT_EQ(entry.project->target, "template-check");
    for (const auto& file : std::filesystem::recursive_directory_iterator(*created)) {
        if (!file.is_regular_file()) {
            continue;
        }
        std::ifstream in(file.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("{{"), std::string::npos) << file.path() << " keeps a placeholder";
    }
}

TEST(ProjectEngine, ReplacesTheValueAndKeepsTheRestOfTheFile) {
    TempDir dir;
    dir.write("game/wind_project.toml",
            "# mine\r\nname = \"Game\"\r\nengine = \"0.1.0a\"  # pinned\r\ntarget = \"game\"\r\n");
    ASSERT_TRUE(launcher::set_project_engine(dir.path() / "game", "0.1.4a"));
    EXPECT_EQ(launcher_test::read_file(dir.path() / "game/wind_project.toml"),
            "# mine\r\nname = \"Game\"\r\nengine = \"0.1.4a\"  # pinned\r\ntarget = \"game\"\r\n");
    const launcher::ProjectEntry entry = launcher::read_project_entry(dir.path() / "game");
    ASSERT_TRUE(entry.project.has_value());
    EXPECT_EQ(entry.project->engine, "0.1.4a");
}

TEST(ProjectEngine, AddsTheKeyWhenThereIsNoneAndSkipsLookAlikes) {
    TempDir dir;
    dir.write("game/wind_project.toml", "name = \"Game\"\nengine_note = \"x\"\ntarget = \"game\"");
    ASSERT_TRUE(launcher::set_project_engine(dir.path() / "game", "0.2.0"));
    EXPECT_EQ(launcher_test::read_file(dir.path() / "game/wind_project.toml"),
            "name = \"Game\"\nengine_note = \"x\"\ntarget = \"game\"\nengine = \"0.2.0\"\n");
}

TEST(ProjectEngine, AMissingFileIsAnError) {
    TempDir dir;
    EXPECT_FALSE(launcher::set_project_engine(dir.path() / "nowhere", "0.2.0"));
}
