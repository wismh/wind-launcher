# Wind Launcher

The launcher of the [Wind engine](https://github.com/wismh/wind-engine): it lists your Wind projects and the editor SDKs installed on your machine, makes a new project from the SDK's template, and opens a project in the editor of the engine version it names. It is itself a Wind game project (`wind_project.toml`, `find_package(Wind)`, `engine_add_game`), built against a released SDK.

## What it does

- **Projects**: lists your Wind projects, makes a new one from an SDK's template, and opens a project in the editor of the engine version its `wind_project.toml` names.
- **SDKs**: lists the editor SDKs installed on this machine (**Installed**) and the ones the engine publishes on GitHub for this system (**Available**). **Install** downloads the SDK, checks its SHA-256, unpacks it, and adds it to **Installed**; a project that needs an SDK you do not have shows **Install `<version>`** instead of Open. **Delete** in the row's `···` menu removes an installed SDK.

## Download

These links always point to the newest launcher (the `latest` release is rebuilt on every release; the version-less names stay the same):

| System | Download |
| --- | --- |
| Windows (x86_64) | [wind-launcher-windows-x86_64.zip](https://github.com/wismh/wind-launcher/releases/latest/download/wind-launcher-windows-x86_64.zip) |
| Linux (x86_64) | [wind-launcher-linux-x86_64.tar.gz](https://github.com/wismh/wind-launcher/releases/latest/download/wind-launcher-linux-x86_64.tar.gz) |
| macOS (Apple silicon, arm64) | [wind-launcher-macos-arm64.tar.gz](https://github.com/wismh/wind-launcher/releases/latest/download/wind-launcher-macos-arm64.tar.gz) |

Every launcher version is also a release of its own, `v<major>.<minor>.<patch><stage>` (`a` alpha, `b` beta, none for production), on the [Releases](https://github.com/wismh/wind-launcher/releases) page, with a `.sha256` file for each archive. The launcher versions are independent of the engine versions. Unpack the archive anywhere and run `wind_launcher` (`wind_launcher.exe`); `assets/` has to stay beside it.

Supported: Windows 10 and later (x86_64), Linux x86_64 with a desktop and OpenGL, macOS 14 and later on Apple silicon.

### Not signed

The launcher is not code-signed or notarized yet:

- **macOS**: Gatekeeper says the app cannot be opened because the developer cannot be verified. Right-click `wind_launcher` and choose **Open**, then **Open** again (or `xattr -d com.apple.quarantine wind_launcher` in the unpacked folder).
- **Windows**: SmartScreen shows "Windows protected your PC". Choose **More info**, then **Run anyway**.

## Installing SDKs

The Available list is the list of releases of [wismh/wind-engine](https://github.com/wismh/wind-engine/releases) that have an SDK archive for this system (`wind-sdk-<version>-<os>-<arch>` with its `.sha256`), newest first, with the stage (alpha, beta, release), the date, and the size. It is kept in the launcher's data directory (`available_sdks.txt`), so it shows at once and when you are offline ("offline: showing the list from 2026-10-10"). **Refresh** asks GitHub again.

An install downloads the checksum file and the archive into a temporary directory inside the install directory, checks the SHA-256, unpacks it, checks that it holds exactly `wind-sdk-<version>-<os>-<arch>/` with an `sdk.toml` of that version and `bin/wind_editor`, and renames it to `sdk-<version>`. Any failure (no network, a damaged download, a bad archive) removes the temporary directory and says why on the row; **Cancel** does the same. One install runs at a time, and the window stays responsive. An install that was cut short (the launcher closed, a power loss) is cleaned up the next time the launcher starts. The archive is never run, and an entry that would leave its folder (`..`, an absolute path, a link out) is refused.

### Modules

**Install** on a row opens a page of what comes with the SDK. The editor SDK is always installed. **Debug symbols** (`wind-sdk-symbols-<version>-<os>-<arch>` with its `.sha256`, 100 MB or more, only for debugging the engine itself) are optional and off by default; a release that has no such package does not offer them. Ticked, the package goes through the same steps after the SDK (checksum, download, SHA-256, unpacking) and is copied over the unpacked SDK before it is renamed to `sdk-<version>`, so the SDK appears whole with its symbols. The package has to be of the SDK's build: its `symbols.toml` must name the same `version` and `build_id` as the SDK's `sdk.toml`, or nothing of it is copied. If only the symbols fail (offline half way, a damaged download, another build), the SDK is installed anyway and the message says what is missing.

An installed SDK without them has **Add debug symbols** in its ··· menu on the SDKs page (the same steps over the installed SDK; the copy is the only part that touches it, and `symbols.toml`, which says the symbols are there, is written last). The SDK's detail line says "debug symbols" once they are in. While anything installs, its progress is at the right end of the status bar on every page.

Downloads go through the engine's HTTP client (WinHTTP on Windows, the system libcurl on Linux and macOS), and the archive is streamed to disk. Unpacking needs `tar` on the `PATH`; it ships with Windows 10 (1803 and later), macOS, and every common Linux, and the launcher says so when it is missing.

GitHub allows 60 unauthenticated API requests an hour. Over that the launcher shows the saved list and a note. Set `GITHUB_TOKEN` or `GH_TOKEN` (any token, no scopes needed) in the environment the launcher starts in to raise the limit; it is sent only to `api.github.com`.

## Where things are stored

| What | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Installed SDKs | `%LOCALAPPDATA%\Programs\Wind\Sdks\<name>` | `$XDG_DATA_HOME/Wind/Sdks` or `~/.local/share/Wind/Sdks` | `~/.local/share/Wind/Sdks` |
| New projects (default folder) | `%USERPROFILE%\WindProjects` | `~/WindProjects` | `~/WindProjects` |
| The launcher's own lists (projects, located SDKs) | the user data directory `Wind/Launcher/launcher.txt` of the operating system | | |

An SDK anywhere else can be added with Locate on the SDKs page.

## Build from source

You need CMake 3.25 or newer, a C++23 compiler (Visual Studio 2026, GCC 13 or newer, or Apple Clang of Xcode 15 or newer), and a released Wind SDK of the version named in `wind_project.toml` (`engine = "..."`). Download the SDK for your system from the [engine releases](https://github.com/wismh/wind-engine/releases) and unpack it. The SDK and the launcher have to be built with the same compiler.

```sh
cmake -S . -B build -G "Ninja Multi-Config" -DWIND_EXPORT=ON \
  -DCMAKE_PREFIX_PATH=/path/to/wind-sdk -DWind_DIR=/path/to/wind-sdk/cmake -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build --config Release
ctest --test-dir build -C Release          # on Linux under xvfb-run, with SDL_AUDIODRIVER=dummy
build/bin/Release/wind_launcher
```

On Windows use `-G "Visual Studio 18 2026"` instead of Ninja. On Linux the SDK needs the same development packages as the engine (X11, Wayland, ALSA, Mesa; the list is in `.github/actions/build/action.yml`). The tests need no network and no display other than the virtual one.

## Tests

`wind_launcher_tests` needs no network and no display: the release parsing against recorded GitHub answers (`tests/fixtures/`), the version order, the ETag cache, SHA-256 against the standard vectors, the installer against a fake downloader and extractor (every failure leaves the SDK list as it was, and the debug symbols are added only to the SDK of their own build), and real archives made with the system `tar` and unpacked by the real extraction path. `tools/e2e_install.py` is the check with a real network: it drives a Development build of the launcher with `wind-cli`, installs a real SDK from GitHub through the window, checks it on disk, and deletes it again (`ci.yml` runs it on Linux; the screenshots of one run are in `docs/e2e/`).

```sh
cmake -S . -B build-dev -G "Ninja Multi-Config" -DWIND_EXPORT=ON -DCMAKE_PREFIX_PATH=/path/to/wind-sdk \
  -DWind_DIR=/path/to/wind-sdk/cmake "-DCMAKE_CONFIGURATION_TYPES=DebugGame;Release"
cmake --build build-dev --config DebugGame --target wind_launcher
xvfb-run -a python3 tools/e2e_install.py --launcher build-dev/bin/DebugGame/wind_launcher --sdk-bin /path/to/wind-sdk/bin --out e2e-out
```

## Releases

Every push to `main` builds, tests, and publishes a release (`.github/workflows/release.yml`). The version is `version.txt` (`0.1a`: major 0, minor 1, alpha) plus a patch counted from the existing tags. `ci.yml` builds and tests every push and pull request on the three systems. `gh workflow run release.yml --ref <branch> -f publish=false` is a dry run that publishes nothing.

## License

Copyright 2026 Yevhenii Tertychnyi. The Wind Launcher is licensed under the [Apache License, Version 2.0](LICENSE) (see also [NOTICE](NOTICE)). It is built against the Wind engine SDK, which is licensed under the same license.
