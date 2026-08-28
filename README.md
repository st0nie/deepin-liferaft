# Deepin Liferaft

<p align="center">
  <img src="data/icons/deepin-liferaft.svg" width="128" alt="Deepin Liferaft icon">
</p>

Deepin Liferaft provides a macOS-style "Your system has run out of application memory" dialog for Deepin. It detects sustained memory pressure, pauses application cgroups before the desktop becomes unusable, and lets the user resume or force quit an application.

## Behavior

- Polls the current `user@UID.service` cgroup every second.
- Uses Fedora's workstation systemd-oomd policy: `full avg10` above 50% for 20 seconds, with reclaim activity (`pgscan`) seen in the last 30 seconds.
- Also triggers when system memory and swap usage both exceed 90% and an application uses more than 5% of total swap.
- Lists up to 10 `app-DDE-*` cgroups by `memory.current`.
- Attributes child resources to the application cgroup. A `memhog` process launched in Deepin Terminal therefore increases the Terminal row.
- Reads localized names and icons from XDG desktop files, including the Linglong export directory at `/var/lib/linglong/entries/apps/share/applications`.
- Ranks pressure candidates by the latest `pgscan` delta and then memory, or ranks swap candidates by `memory.swap.current`.
- Freezes up to three candidates with `cgroup.freeze`; Resume thaws the selected group and Force Quit uses `cgroup.kill`.
- Waits 15 seconds after an action before showing another dialog.

Detection follows Fedora's systemd-oomd policy while interaction follows macOS. systemd-oomd kills one cgroup immediately; Deepin Liferaft pauses up to three candidates and leaves the final choice to the user.

## Whitelist

Whitelisted applications never appear in the dialog's application list and are never frozen or force quit. Entries are desktop file IDs, the same IDs parsed from cgroup unit names — for example `google-chrome` matches `app-DDE-google\x2dchrome@....service`. Write one ID per line; blank lines and `#` comments are ignored.

Following the vim configuration model, three levels are merged into one set:

| Level | File |
| --- | --- |
| Package defaults | `/usr/share/deepin-liferaft/whitelist` |
| System administrator | `/etc/deepin-liferaft/whitelist` |
| Per-user | `~/.config/deepin-liferaft/whitelist` |

The packaged defaults protect the monitor itself, the screen locker (`dde-lock`), and the desktop shell (`dde-shell`). The whitelist is loaded once at startup, so restart the service after editing:

```bash
systemctl --user restart deepin-liferaft.service
```

The startup log records how many entries were loaded.

## Safety

Deepin Liferaft only thaws cgroups it froze itself. It skips cgroups already frozen by another component and never freezes the cgroup containing its own process.

Closing the dialog thaws every owned cgroup before the window closes. If a thaw temporarily fails, the window remains open and retries. `SIGTERM` and `SIGINT`, including `systemctl --user stop`, are received through `signalfd` so owned cgroups are thawed before process exit.

Invalid PSI, `memory.stat`, `memory.current`, or `/proc/meminfo` samples do not trigger an action.

The systemd user service sets `MemoryMin=16M`, which gives the monitor's own cgroup hard reclaim protection: the kernel will not swap out or reclaim its protected pages even under extreme memory pressure, so the daemon can keep polling PSI and raise the dialog exactly when the rest of the system is starving. 16 MiB covers the measured hidden-mode working set (about 13 MiB PSS) with margin. cgroup v2 caps a leaf cgroup's effective protection by its ancestors' `memory.min`; Deepin sessions already protect the whole `user.slice` chain, so the leaf protection is effective there. This protection applies to service mode and requires systemd 253 or newer (older versions ignore the unknown property); manual foreground runs from a terminal have no such protection.

## Logging

Deepin Liferaft logs through DTK's `DLog` (journald appender). Under the systemd user service messages go to the user journal; run from a terminal, they also appear on stderr. Log lines carry a timestamp, level, source file, function, and line.

Inspect the daemon's status:

```bash
journalctl --user -u deepin-liferaft.service -f
```

The journal records program startup (pid and hidden/foreground mode), pressure threshold crossings, trigger decisions with pressure, duration, reclaim and memory usage, every freeze (cgroup, trigger, sort metric), every force quit (kill/thaw result), resume and thaw failures, and shutdown cleanup. Invalid samples are logged as warnings on state change rather than every second.

The service unit overrides the DDE session default `QT_LOGGING_RULES` (which suppresses `qInfo` in journald) so that Info-level messages reach the journal:

```bash
systemctl --user daemon-reload
systemctl --user restart deepin-liferaft.service
```

## Resource Use

`--hidden` mode delays creation of the table, labels, buttons, and icon-theme data until the dialog is first shown. It also avoids scanning every application cgroup while pressure is low and the window is hidden.

On the development machine, measured three seconds after startup:

| Metric | Before | After |
| --- | ---: | ---: |
| RSS | 60.7 MiB | 52.7 MiB |
| PSS | 19.4 MiB | 13.0 MiB |
| Private dirty | 9.85 MiB | 7.93 MiB |

Most remaining RSS is shared DTK/Qt code. An otherwise empty `DApplication` measured about 41.6 MiB RSS on the same machine, so RSS alone overstates private memory cost.

## Requirements

- Linux cgroup v2 with `memory`, `memory.swap`, `memory.pressure`, `cgroup.freeze`, and `cgroup.kill`
- Deepin sessions that place applications in `app-DDE-*` cgroups
- Qt 6 and DTK 6 Widget development packages
- Qt 6 Linguist tools (`qt6-tools-dev`, `qt6-l10n-tools`) to compile translations
- CMake 3.16 or newer and a C++17 compiler

## Build and Test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The self-test covers PSI parsing, pressure and swap threshold boundaries, `pgscan` sampling failures, memory formatting, freezer ownership/thaw behavior, and whitelist parsing, merging, and application filtering.

Run visibly for UI inspection:

```bash
./build/deepin-liferaft
```

Run as a hidden monitor:

```bash
./build/deepin-liferaft --hidden
```

## Debian Package

```bash
dpkg-buildpackage -us -uc -b
sudo apt install ../deepin-liferaft_0.1.0_amd64.deb
```

The package installs:

- `/usr/bin/deepin-liferaft`
- `/usr/lib/systemd/user/deepin-liferaft.service`
- `/usr/share/applications/deepin-liferaft.desktop`
- `/usr/share/icons/hicolor/scalable/apps/deepin-liferaft.svg`
- `/usr/share/doc/deepin-liferaft/README.md.gz`
- `/usr/share/man/man1/deepin-liferaft.1.gz`
- `/usr/share/deepin-liferaft/whitelist` with the packaged whitelist defaults
- `/usr/share/deepin-liferaft/translations/` with `zh_CN`, `en_US`, `ja_JP`, and `ko_KR` message catalogs

## User Service

The service starts the monitor with `--hidden` in graphical sessions. Debian debhelper enables user units globally, so a per-user `disable` does not override the global enable link.

Disable only for the current user:

```bash
systemctl --user mask --now deepin-liferaft.service
```

Restore it:

```bash
systemctl --user unmask deepin-liferaft.service
systemctl --user enable --now deepin-liferaft.service
```

Disable global enable for all users, then stop the current session instance:

```bash
sudo systemctl --global disable deepin-liferaft.service
systemctl --user stop deepin-liferaft.service
```

Inspect status:

```bash
systemctl --user status deepin-liferaft.service
```

## Platform Differences

Linux PSI and systemd cgroups replace macOS VM-pressure and application lifecycle APIs. The dialog is therefore behaviorally similar rather than binary-identical: DDE cgroups define application boundaries, Fedora's systemd-oomd thresholds decide when to show the dialog, and DTK supplies native Deepin window styling.

## License

Deepin Liferaft is licensed under [GPL-3.0-or-later](LICENSE).
