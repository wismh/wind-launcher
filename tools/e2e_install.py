#!/usr/bin/env python3
"""Installs a real SDK through the launcher's own window and removes it again, driving the launcher with wind-cli.

  xvfb-run -a python3 tools/e2e_install.py --launcher build-dev/bin/DebugGame/wind_launcher \
      --sdk-bin /path/to/wind-sdk/bin --out e2e-out [--version 0.1.1a]

The launcher has to be a Development build (the DebugGame configuration of the export build): only that has the CLI
server wind-cli talks to. The launcher runs with a temporary home, so it reads and writes nothing of the real user's.
It opens the SDKs page and its Install SDK page, waits for the Available list from GitHub, presses Install on a row
(default: the newest), then Install on the modules page (with --symbols, after ticking Debug symbols), waits until the
row says Installed, checks sdk-<version>/ on disk (sdk.toml, the editor, `wind_editor --batch` printing its usage error,
and symbols.toml of the same build with --symbols, none without), then deletes the SDK from the Installed list and checks
that the directory is gone. Screenshots of the modules page and of the page before, during, and after the install, and
after the delete, go to --out. Exit code 0 when all of it held.

Needs network access to api.github.com and github.com. GITHUB_TOKEN or GH_TOKEN in the environment lifts the API's rate
limit, as it does for the launcher. Standard library only.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ITEM = 2147483648  # the path index of the first row of a virtualized list

INSTALLED = [0, 1, 1, 1, 0]
AVAILABLE = [0, 1, 2, 1, 0]
INSTALL_SDK_BUTTON = [0, 1, 1, 0, 3]  # "Install SDK…" on the SDKs page, which opens the Install SDK page
BACK_BUTTON = [0, 1, 2, 0, 2]  # "Back" on the Install SDK page
OPTIONS_SYMBOLS = [0, 1, 3, 1, 1]  # the Debug symbols checkbox on the modules page, which Install on a row opens
OPTIONS_INSTALL = [0, 1, 3, 1, 3, 1]  # "Install" on the modules page
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+[ab]?$")


class Failure(Exception):
    pass


class Cli:
    def __init__(self, exe, env):
        self.exe, self.env = exe, env

    def run(self, *args, check=True, timeout=30):
        done = subprocess.run([self.exe, *args], env=self.env, capture_output=True, text=True, timeout=timeout)
        try:
            body = json.loads(done.stdout)
        except ValueError:
            body = {"ok": False, "error": done.stdout + done.stderr}
        if check and not body.get("ok"):
            raise Failure("wind-cli %s: %s" % (" ".join(args), body.get("error", body)))
        return body

    def nodes(self):
        return self.run("tree")["result"]["nodes"]

    def element(self, path):
        return self.run("element", "path:" + "/".join(str(p) for p in path))["result"]

    def text(self, path):
        body = self.run("element", "path:" + "/".join(str(p) for p in path), check=False)
        return body.get("result", {}).get("text") if body.get("ok") else None

    def click(self, path):
        result = self.run("click", "path:" + "/".join(str(p) for p in path))["result"]
        if not result.get("executed"):
            raise Failure("click %s did not run: %s" % (path, result.get("reason")))

    def shot(self, out_dir, name):
        target = Path(out_dir) / name
        self.run("screenshot", "--out", str(target))
        print("screenshot", target, flush=True)


def log(text):
    print(text, flush=True)


def wait_for(what, check, timeout, interval=0.3):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = check()
        except Failure as error:
            value, last = None, error
        if value:
            return value
        time.sleep(interval)
    raise Failure("timeout waiting for %s%s" % (what, ": %s" % last if last else ""))


def rows(cli, base):
    """(item index, version text) of the rows of a list that are laid out, in order."""
    items = sorted({n["path"][len(base)] for n in cli.nodes()
                    if n["kind"] == "Label" and "version" in n["classes"] and n["path"][:len(base)] == base
                    and len(n["path"]) > len(base) + 1 and n["path"][len(base)] >= ITEM and n["border"]["w"] > 0})
    found = []
    for item in items:
        text = cli.text(base + [item, 0])
        if text:
            found.append((item, text))
    return found


def environment(root):
    env = dict(os.environ)
    home = Path(root) / "home"
    data = home / ".local" / "share"
    runtime = Path(root) / "run"
    for directory in (home, data, runtime):
        directory.mkdir(parents=True, exist_ok=True)
    runtime.chmod(0o700)
    env.update(HOME=str(home), XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(home / ".config"),
               XDG_CACHE_HOME=str(home / ".cache"), XDG_RUNTIME_DIR=str(runtime),
               LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
    if os.name == "nt":
        env.update(USERPROFILE=str(home), LOCALAPPDATA=str(home / "AppData" / "Local"))
        install = home / "AppData" / "Local" / "Programs" / "Wind" / "Sdks"
    else:
        install = data / "Wind" / "Sdks"
    return env, install


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--launcher", required=True, help="a Development build of wind_launcher")
    parser.add_argument("--sdk-bin", required=True, help="the bin/ of an SDK: wind-cli, and the editor to try")
    parser.add_argument("--out", required=True, help="where the screenshots go")
    parser.add_argument("--version", help="the SDK to install; default the newest listed")
    parser.add_argument("--symbols", action="store_true", help="install the debug symbols with it (a download of 100 MB)")
    parser.add_argument("--add-symbols", action="store_true",
                        help="after the install, add the debug symbols from the SDK's ··· menu (another 100 MB)")
    parser.add_argument("--timeout", type=int, default=900, help="seconds for the download and unpacking")
    args = parser.parse_args(argv)

    exe = ".exe" if os.name == "nt" else ""
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    root = tempfile.mkdtemp(prefix="wind-e2e-")
    env, install_dir = environment(root)
    cli = Cli(str(Path(args.sdk_bin) / ("wind-cli" + exe)), env)
    launcher = subprocess.Popen([args.launcher], env=env, stdout=open(Path(root) / "launcher.log", "w"),
                                stderr=subprocess.STDOUT)
    try:
        wait_for("the launcher's CLI", lambda: cli.run("status", check=False).get("result"), 60)
        log("launcher is up (pid %d), home %s" % (launcher.pid, root))

        cli.click([0, 0, 2])  # the SDKs tab
        cli.click(INSTALL_SDK_BUTTON)
        available = wait_for("the Available list", lambda: rows(cli, AVAILABLE), 90)
        log("available: %s" % ", ".join(v for _, v in available))
        item, version = next(((i, v) for i, v in available if args.version in (None, v)), (None, None))
        if item is None:
            raise Failure("%s is not in the Available list %s" % (args.version, [v for _, v in available]))
        if not VERSION.match(version):
            raise Failure("%r is not a version" % version)
        if (install_dir / ("sdk-" + version)).exists():
            raise Failure("the temporary home already has the SDK")
        cli.shot(out, "1-before-install.png")

        # Install, and watch the row's state line.
        cli.click(AVAILABLE + [item, 2])
        log("pressed Install on %s" % version)
        wait_for("the modules page", lambda: cli.text(OPTIONS_INSTALL) == "Install", 15)
        if args.symbols:
            cli.click(OPTIONS_SYMBOLS)
        cli.shot(out, "0-modules.png")
        cli.click(OPTIONS_INSTALL)
        seen = []
        shot_during = False
        deadline = time.monotonic() + args.timeout
        while True:
            state = cli.text(AVAILABLE + [item, 1, 1]) or ""
            if state and (not seen or seen[-1] != state.split(" ")[0]):
                seen.append(state.split(" ")[0])
                log("state: %s" % state)
            if not shot_during and (state.startswith("Downloading") or state.startswith("Checking the download")
                                    or state.startswith("Unpacking")):
                cli.shot(out, "2-during-install.png")
                shot_during = True
            if state == "Installed":
                break
            if time.monotonic() > deadline:
                raise Failure("the install did not finish in %d s (last state: %r)" % (args.timeout, state))
            if state and not state.startswith(("Getting", "Downloading", "Checking", "Unpacking", "Debug symbols")):
                raise Failure("the install failed: %s" % state)
            time.sleep(0.15)
        cli.shot(out, "3-after-install.png")
        if not shot_during:
            log("note: the install was too fast for a screenshot of the progress")

        # On disk.
        sdk = install_dir / ("sdk-" + version)
        manifest = (sdk / "sdk.toml").read_text(encoding="utf-8")
        if 'version = "%s"' % version not in manifest:
            raise Failure("sdk.toml has another version:\n" + manifest)
        editor = sdk / "bin" / ("wind_editor" + exe)
        if not editor.is_file():
            raise Failure("no editor at %s" % editor)
        tried = subprocess.run([str(editor), "--batch"], env=env, capture_output=True, text=True, timeout=60)
        if tried.returncode != 2:
            raise Failure("wind_editor --batch exited with %d, not 2: %s%s" % (tried.returncode, tried.stdout, tried.stderr))
        log("on disk: %s, editor usage error: %s" % (sdk, (tried.stderr or tried.stdout).strip().splitlines()[0]))
        marker = sdk / "symbols.toml"
        if args.symbols:
            if not marker.is_file():
                raise Failure("no %s: the debug symbols were not installed" % marker)
            build_ids = [re.search(r'^build_id\s*=\s*"([^"]*)"', path.read_text(encoding="utf-8"), re.M)
                         for path in (sdk / "sdk.toml", marker)]
            if not all(build_ids) or build_ids[0].group(1) != build_ids[1].group(1):
                raise Failure("the symbols are not of the SDK's build")
            log("debug symbols: %s is of the SDK's build" % marker.name)
        elif marker.exists():
            raise Failure("%s is there although the symbols were not asked for" % marker)
        leftovers = [p.name for p in install_dir.iterdir() if p.name != "sdk-" + version]
        if leftovers:
            raise Failure("left in the install directory: %s" % leftovers)

        # The Installed list shows it; delete it from there.
        cli.click(BACK_BUTTON)
        installed = wait_for("the Installed list", lambda: rows(cli, INSTALLED), 30)
        row = next(i for i, v in installed if v == version)
        if args.add_symbols:
            if args.symbols:
                raise Failure("--add-symbols is for an SDK installed without them")
            cli.click(INSTALLED + [row, 2])  # the ··· button
            time.sleep(0.5)
            label = cli.text(INSTALLED + [row, 2, 0, 1]) or ""
            if not label.startswith("Add debug symbols"):
                raise Failure("the ··· menu offers %r, not Add debug symbols" % label)
            cli.click(INSTALLED + [row, 2, 0, 1])  # Add debug symbols
            log("pressed %s" % label)
            wait_for("the symbols to be added", lambda: (sdk / "symbols.toml").is_file(), args.timeout)
            detail = wait_for("the SDK row to say it has them",
                              lambda: "debug symbols" in (cli.text(INSTALLED + [row, 1, 0]) or ""), 30)
            log("added: %s" % detail)
            cli.shot(out, "5-symbols-added.png")
            installed = wait_for("the Installed list", lambda: rows(cli, INSTALLED), 30)
            row = next(i for i, v in installed if v == version)
        cli.click(INSTALLED + [row, 2])  # the ··· button
        time.sleep(0.5)
        cli.click(INSTALLED + [row, 2, 0, 2])  # Delete…
        time.sleep(0.5)
        cli.click(INSTALLED + [row, 2, 1, 2, 1])  # Delete, in the confirmation
        wait_for("the SDK directory to go", lambda: not sdk.exists(), 60)
        wait_for("the Installed list to be empty", lambda: not rows(cli, INSTALLED), 30)
        cli.shot(out, "4-after-uninstall.png")
        log("uninstalled: %s is gone" % sdk)
        log("E2E OK: installed and removed SDK %s through the launcher" % version)
        return 0
    except Failure as error:
        log("E2E FAILED: %s" % error)
        try:
            cli.shot(out, "failure.png")
        except Exception:  # noqa: BLE001 - the launcher may be gone
            pass
        return 1
    finally:
        launcher.terminate()
        try:
            launcher.wait(timeout=15)
        except subprocess.TimeoutExpired:
            launcher.kill()
        shutil.copy(Path(root) / "launcher.log", out / "launcher.log")
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
