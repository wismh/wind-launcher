#pragma once

#include "method_command.h"

#include <engine/ui/bindable.h>
#include <engine/ui/view_model.h>

#include <cstddef>
#include <string>

namespace launcher {

class LauncherApp;

// One SDK row (`sdks` in assets/ui/launcher.xml). The ··· button opens a menu: Show in Explorer, then Forget for a
// located SDK or Delete… for an installed one. Delete… swaps the menu for a confirmation on the same anchor. The menu
// and the confirmation are the row's own state; what changes the lists goes to the launcher as a request.
class SdkRowViewModel final : public engine::ui::ViewModel {
public:
    SdkRowViewModel(LauncherApp& app, std::size_t index, bool located);

    SdkRowViewModel(const SdkRowViewModel&) = delete;
    SdkRowViewModel& operator=(const SdkRowViewModel&) = delete;

    void toggle_menu();
    void show_sdk();
    // Add debug symbols: closes the menu and asks the launcher to download them.
    void add_symbols();
    // Forget for a located SDK; for an installed one, opens the confirmation.
    void remove_sdk();
    void delete_sdk();
    void cancel_delete();

    engine::ui::Bindable<std::string> version;
    // Configuration, dirty, short commit, and whether it was located or installed.
    engine::ui::Bindable<std::string> detail;
    engine::ui::Bindable<std::string> path;
    // "Forget" or "Delete…".
    engine::ui::Bindable<std::string> removeLabel;
    engine::ui::Bindable<std::string> confirmText;
    // The menu item "Add debug symbols (85.1 MB)": shown for an installed SDK without them whose release has a package,
    // while nothing else installs.
    engine::ui::Bindable<bool> canAddSymbols;
    engine::ui::Bindable<std::string> symbolsLabel;
    engine::ui::Bindable<bool> menuOpen;
    engine::ui::Bindable<bool> confirmOpen;
    MethodCommand toggleMenu;
    MethodCommand show;
    MethodCommand addSymbols;
    MethodCommand remove;
    MethodCommand confirmDelete;
    MethodCommand cancelDelete;

private:
    LauncherApp* app_;
    std::size_t index_;
    bool located_;
};

}
