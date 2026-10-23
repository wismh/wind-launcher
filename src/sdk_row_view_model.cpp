#include "sdk_row_view_model.h"

#include "launcher_app.h"

#include <asset_ids.h>

namespace launcher {

SdkRowViewModel::SdkRowViewModel(LauncherApp& app, std::size_t index, bool located)
    : removeLabel(std::string(located ? "Forget" : "Delete…"))
    , app_(&app)
    , index_(index)
    , located_(located) {
    assets::ui::Launcher::Sdks::bind(*this);
    toggleMenu.bind_to<SdkRowViewModel, &SdkRowViewModel::toggle_menu>(*this);
    show.bind_to<SdkRowViewModel, &SdkRowViewModel::show_sdk>(*this);
    addSymbols.bind_to<SdkRowViewModel, &SdkRowViewModel::add_symbols>(*this);
    remove.bind_to<SdkRowViewModel, &SdkRowViewModel::remove_sdk>(*this);
    confirmDelete.bind_to<SdkRowViewModel, &SdkRowViewModel::delete_sdk>(*this);
    cancelDelete.bind_to<SdkRowViewModel, &SdkRowViewModel::cancel_delete>(*this);
}

void SdkRowViewModel::toggle_menu() {
    // The anchor click closes whichever of the two is open; only a click on a closed anchor opens the menu.
    if (confirmOpen.get()) {
        confirmOpen = false;
        return;
    }
    menuOpen = !menuOpen.get();
}

void SdkRowViewModel::show_sdk() {
    menuOpen = false;
    app_->request(LauncherRequest{.kind = LauncherRequest::Kind::ShowSdk, .index = index_});
}

void SdkRowViewModel::add_symbols() {
    menuOpen = false;
    app_->request(LauncherRequest{.kind = LauncherRequest::Kind::AddSymbols, .index = index_});
}

void SdkRowViewModel::remove_sdk() {
    menuOpen = false;
    if (located_) {
        app_->request(LauncherRequest{.kind = LauncherRequest::Kind::ForgetSdk, .index = index_});
        return;
    }
    confirmText = "Delete SDK " + version.get() + " and all its files?";
    confirmOpen = true;
}

void SdkRowViewModel::delete_sdk() {
    confirmOpen = false;
    app_->request(LauncherRequest{.kind = LauncherRequest::Kind::DeleteSdk, .index = index_});
}

void SdkRowViewModel::cancel_delete() {
    confirmOpen = false;
}

}
