# Something Qt

Latest portable build: [`out/portable/SomethingQt-0.1.0-win64-smart-schedule-fix-20261031.zip`](out/portable/SomethingQt-0.1.0-win64-smart-schedule-fix-20261031.zip). It includes the Smart Schedule preview status binding fix. Release C++/QML compilation, Qt deployment, and static ZIP checks passed (145 files and 22 runtime requirements); tests and application launch were not run for this artifact. Extract the entire ZIP, run `bin/SomethingQt.exe`, and enter `run full` in Console to open the main window. Profiles use schema 19 and remain in the normal Windows application-data location. Full visual and clean-machine acceptance remain pending. See the [portable build record](docs/portable-build.md) and [window-size repair](docs/window-size-fix-20261030.md).

An independent C++20 and Qt Quick rewrite of Something. The shell owns a lifecycle-managed service graph with per-profile locking, GUI-thread operation coordination, bounded CPU jobs, a dedicated SQLite worker, and a dedicated direct-HTTPS worker. It also supports Windows tray presentation and an optional hidden background start. Retained behavior is specified in [`docs/feature-parity.md`](docs/feature-parity.md).

## Technology baseline

- C++20 with CMake 3.21 or newer.
- Qt 6.5 or newer, using Core, Gui, Widgets, Qml, Quick, Quick Controls 2, Quick Dialogs 2, Network, and Sql.
- The portable testing build uses Qt 6.11.0 for 64-bit MinGW, paired with the Qt-provided MinGW-w64 GCC 13.1.0 toolchain and CMake 3.30.5. Configuration, compilation and deployment succeeded with that kit. Details are in [`docs/toolchain.md`](docs/toolchain.md).
- Qt package discovery is supplied by the user's Qt kit/environment. No machine-specific Qt, compiler, or CMake path is embedded in project files.

Qt Gui provides raster image APIs used by Qt Quick's image support; available formats depend on the selected runtime's image-format plugins. The inspected kit contains GIF, ICO, JPEG, and SVG plugins, plus SQLite. No WebP image-format plugin was observed. Wallpaper import accepts only images Qt can decode; WebP decoding therefore remains a toolchain/deployment item to resolve.

## Project layout

```text
src/
  application/   Application composition and lifecycle
  domain/        Domain types and validation rules
  services/      Feature use cases and workflows
  storage/       SQLite repositories and migrations
  providers/     Direct external-provider clients and parsers
  platform/      Native operating-system adapters
  models/        QML-facing state models
  controllers/   Presentation orchestration
qml/
  shell/         Application shell
  components/    Shared QML components
  pages/         Retained feature pages
  dialogs/       Reusable dialogs
  theme/         Shared visual tokens and styles
assets/          Project-owned static assets
tests/           Opt-in, test-only targets
docs/            Planning and toolchain documentation
```

## Dependency and scope rules

- QML presents state and user actions. It does not issue SQL or call providers/native APIs directly.
- Runtime Qt dependencies are declared on the `SomethingQt` target. Qt Test is discovered only when `SOMETHING_QT_BUILD_TESTS` is enabled and is not linked into the application.
- There is no localhost HTTP server, browser engine, Go helper, or excluded feature integration.
- The QML shell and wallpaper manager are in place. Built-in shell artwork is bundled as Qt resources; imported wallpaper files live under this application's own profile directory and are never written into the reference repository.
- Navigation exposes 13 retained destinations: Dashboard at the hub and 12 orbit entries. Retained feature pages have source-level integrations; desktop/runtime behavior remains unverified.

## One-time profile import

The local database is SQLite through Qt's QSQLITE driver. On Windows the default file is `%LOCALAPPDATA%\Something\Something\profiles\default\something.sqlite`, outside the executable/portable folder. Replacing or moving the app does not reset that profile.

Settings offers an explicit import of Something-v2 schema 24 or 25 into an empty Qt profile. Close Something-v2 completely before selecting its `database.db`; the importer snapshots the database and existing WAL sidecars using read-only file handles. It maps only retained rows/settings/assets, records skips and failures, and commits a completion marker with the target transaction. Successful import requires closing and restarting Something to hydrate feature models. See [`docs/data-import.md`](docs/data-import.md) for the exact supported contract, recovery behavior, and unrun acceptance checks. No import against real data has been performed or authorized.

A Windows x64 Release build and portable deployment have succeeded. The latest review package passed all 23 automated suites using disposable fixtures. It was not launched interactively. See [`docs/review-20261030.md`](docs/review-20261030.md) for executed checks and [`docs/verification.md`](docs/verification.md) for the wider acceptance criteria that remain pending.

## Windows deployment

The project has a configurable Windows install/deployment path using Qt's QML-aware CMake deployment helper. It produces a relocatable application folder. User profiles remain in the Qt application data location outside that tree. A Release testing ZIP has been built and checked statically; see [`docs/windows-deployment.md`](docs/windows-deployment.md) for dependency accounting, profile/upgrade behavior and unrun clean-machine acceptance checks.
