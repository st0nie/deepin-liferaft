# AGENTS.md

## Scope

These instructions apply to the entire `deepin-liferaft` repository.

## Project

Deepin Liferaft is a single-binary DTK 6 application. A systemd user service runs it with `--hidden`; sustained Fedora-style systemd-oomd pressure or swap conditions open a DTK force-quit dialog. Application boundaries are DDE `app-DDE-*` launch groups and `app-*.scope` scopes, both under `user@UID.service/app.slice`.

## Layout

- `main.cpp`: monitoring policy, cgroup ownership, lifecycle cleanup, and DTK UI
- `CMakeLists.txt`: build, CTest, translations, resources, and installation
- `resources.qrc`: embedded application resources
- `data/icons/`: application icon source
- `data/whitelist`: packaged whitelist defaults (package level of the three-level merge)
- `translations/`: Qt translation sources (`.ts`) for `zh_CN`, `en_US`, `ja_JP`, `ko_KR`
- `deepin-liferaft.desktop`: DDE application entry
- `deepin-liferaft.service`: systemd user service
- `deepin-liferaft.1`: command manual page
- `debian/`: Debian packaging
- `README.md`: user and operator documentation
- `LICENSE`: GPL-3.0-or-later

Do not edit or commit generated content under `obj-*/` or `build/`, `debian/deepin-liferaft/`, debhelper stamp/helper files, `debian/files`, package artifacts (`.deb`, `.buildinfo`, `.changes`, `*.tar.xz`), `deepin-liferaft-*.tar.gz` tarballs, screenshots (`*.png`), logs, local CodeGraph databases, or `.pi-subagents/`/`progress.md` scratch state.

## Safety Invariants

Memory-pressure code is safety critical. Preserve all of these invariants:

- Parse cgroup `memory.pressure` `full avg10`, not `some`.
- Treat failed or incomplete PSI, `memory.stat`, cgroup value, and `/proc/meminfo` reads as invalid samples. Never convert failure into zero usage or a trigger.
- Never freeze the cgroup containing Deepin Liferaft.
- Read `cgroup.freeze` before freezing. Do not claim or thaw a cgroup already frozen by another component.
- Track every cgroup successfully frozen by this process.
- Resume all owned cgroups before accepting a window close, normal process exit, `SIGTERM`, or `SIGINT`.
- If thaw fails, retain ownership and retry. Never silently clear the recovery set.
- After `cgroup.kill`, thaw a surviving cgroup before releasing ownership.
- Keep frozen applications visible in the list even when they fall outside the normal top-ten memory list.

Do not test freeze/kill behavior on real user applications. Use a disposable systemd user cgroup or temporary fake control files.

The systemd user service sets `MemoryMin=16M` so the monitor's own cgroup keeps hard reclaim protection and can still poll PSI under extreme pressure (requires systemd 253+; older versions ignore the property). Preserve it when editing `deepin-liferaft.service`.

## Policy

Current workstation policy:

- poll interval: 1 second
- pressure: `full avg10 > 50%` for 20 seconds
- reclaim must have occurred in the previous 30 seconds
- system memory and swap trigger: both above 90%
- swap candidate: above 5% of total swap
- post-action delay: 15 seconds

Pressure candidates sort by latest `pgscan` delta, then `memory.current`. Swap candidates sort by `memory.swap.current`. The UI may freeze up to three candidates; it does not automatically kill them.

If policy values change, update `README.md`, tests, and Debian package description in the same change.

## Whitelist

Whitelisted applications never appear in the dialog and are never frozen or force quit. Entries are desktop file IDs — the same IDs parsed from cgroup unit names (for example `google-chrome` matches `app-DDE-google\x2dchrome@....service`). One ID per line; blank lines and `#` comments are ignored.

Three levels merge as one set (vim-style):

- Package defaults: `/usr/share/deepin-liferaft/whitelist` (built from `data/whitelist`)
- Administrator: `/etc/deepin-liferaft/whitelist`
- Per-user: `~/.config/deepin-liferaft/whitelist`

The packaged defaults protect the monitor itself (`deepin-liferaft`), the screen locker (`dde-lock`), and the desktop shell (`dde-shell`). The whitelist is loaded once at startup, so restart the service after editing. `parseWhitelist`, `loadWhitelist`, and the `appProcs` whitelist filter are covered by `--self-test`.

## Resource Constraints

Keep hidden mode cheap:

- Do not construct list widgets, labels, buttons, or load desktop icons before the dialog is shown.
- Do not scan all application cgroups while pressure is low and the dialog is hidden.
- Cache desktop metadata; it is static for the lifetime of the daemon.
- Do not add a dependency for parsing or logic available through Qt, libc, procfs, or cgroup v2.

Measure changes with `/proc/PID/smaps_rollup`, not RSS alone. Report PSS and private dirty memory using equal startup timing.

## UI Style

The dialog follows Deepin's design language, not macOS alert styling. Preserve these conventions:

- Use DTK widgets and the DTK palette (`DPalette`, `DPaletteHelper`) and font sizes (`DFontSizeManager`). Never hard-code text or background colors, and never leave margins or a layout that only suits one theme; the dialog must follow light and dark themes and the system accent color automatically.
- The application list is a `DListView` with the `AppRowDelegate` (`DStyledItemDelegate`) that draws the row background and content itself, copying `BaseTableView::drawRow()` from deepin-system-monitor: even rows use `DPalette::AlternateBase`, odd rows `DPalette::Base`, the selected row `DPalette::Highlight`, a hovered row `DStyle::adjustColor(base, 0, 0, -10)`, all rounded by `DStyle::PM_FrameRadius`. `WA_Hover` is enabled on the viewport so rows highlight under the pointer. Update rows in the model instead of rebuilding the list while the dialog is open.
- Window chrome comes from `DMainWindow`/`DTitlebar`. The alert header is the rounded `AlertBanner` with its `AlertBadge`; the footer is a `DHorizontalLine` above right-aligned `Resume` (`DPushButton`) and `Force Quit` (`DWarningButton`) buttons.
- For visual checks, `w.grab()` on `QT_QPA_PLATFORM=offscreen` captures the client area, but DTK resolves its palette from the platform theme, which the offscreen plugin does not provide. Set `DGuiApplicationHelper::standardPalette(LightType | DarkType)` with both `setApplicationPalette()` and `qApp->setPalette()` before showing the window to get a faithful light or dark snapshot.

## Translations

User-visible strings use Qt `tr()` / `QCoreApplication::translate()` so they are translatable. Source `.ts` files live in `translations/` (`zh_CN`, `en_US`, `ja_JP`, `ko_KR`); CMake compiles them to `.qm` via `qt6_add_lrelease` and installs them to `share/deepin-liferaft/translations/`. Building requires Qt 6 Linguist tools (`qt6-tools-dev`, `qt6-l10n-tools`). After adding or changing user-visible strings, regenerate the `.ts` files with `cd translations && /usr/lib/qt6/bin/lupdate -no-obsolete ../main.cpp -ts *.ts` and translate the new entries in every catalog before committing.

`DApplication` resolves catalogs from `/usr/share/deepin-liferaft/translations` (and `~/.local/share/deepin-liferaft/translations`) before the executable directory, so a binary built in `obj-*/` keeps using the installed catalogs. Copy the rebuilt `.qm` files into `~/.local/share/deepin-liferaft/translations/`, or install the package, to see new strings in a local run.

## Logging

Logging goes through DTK `DLog` with journald and console appenders; under the user service, messages land in the user journal. Use `qInfo` for normal state changes (startup, threshold crossings, trigger decisions, freeze/kill, resume, shutdown) and `qWarning` for failures and invalid samples. The service unit sets `QT_LOGGING_RULES=*.info=true` so Info-level messages reach journald; preserve it.

## Build and Test

Run after every functional change:

```bash
cmake -S . -B obj-x86_64-linux-gnu
cmake --build obj-x86_64-linux-gnu --clean-first -j"$(nproc)"
ctest --test-dir obj-x86_64-linux-gnu --output-on-failure
./obj-x86_64-linux-gnu/deepin-liferaft --self-test
```

UI smoke test in a graphical session:

```bash
env DISPLAY=:0 ./obj-x86_64-linux-gnu/deepin-liferaft
```

Verify the window title, application icon, list layout, selection, Resume state, Force Quit behavior, and close cleanup. Verify hidden mode creates no window.

Verify signal cleanup by starting `--hidden`, sending `SIGTERM`, and checking shell wait status is `0`.

## Packaging

Build the Debian package with:

```bash
dpkg-buildpackage -us -uc -b
```

Inspect the package for the binary, user service, desktop entry, scalable icon, packaged whitelist defaults, compiled translation catalogs, README, manual page, and generated debhelper service enable scripts. Run `systemd-analyze --user verify` on the packaged unit.

## Git

Keep commits scoped. Do not include build directories, generated Debian state, `.deb` files, tarballs, logs, temporary screenshots, local CodeGraph state, or Pi scratch state (`.pi-subagents/`, `progress.md`). Before committing, inspect `git status`, staged diff, tests, package contents, and remote configuration.
