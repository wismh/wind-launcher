#pragma once

#include <cstddef>
#include <string>

namespace launcher {

// What a button asked for. The launcher acts on it in its frame system, never inside the UI pass that ran the
// command: acting rebuilds the rows, and the row whose button ran would go away under it.
struct LauncherRequest {
    enum class Kind {
        None,
        AddProject,
        LocateSdk,
        ShowSdkFolder,
        BrowseLocation,
        CreateProject,
        Open,
        Remove,
        ChangeProjectSdk,
        ShowSdk,
        ForgetSdk,
        DeleteSdk,
        // Install on an Available row: opens the modules page for it.
        InstallSdk,
        // Install on the modules page: the SDK with the modules ticked there.
        ConfirmInstall,
        // Add debug symbols on an SDK row.
        AddSymbols,
        InstallVersion,
        CancelInstall,
        RefreshAvailable,
    };

    Kind kind = Kind::None;
    // The project row (Open, Remove, ChangeProjectSdk), SDK row (ShowSdk, ForgetSdk, DeleteSdk, AddSymbols), or Available
    // row (InstallSdk).
    std::size_t index = 0;
    // InstallVersion: the version a project needs, by exact text. ChangeProjectSdk: the version to write into it.
    std::string version;
};

}
