# Repository instructions

## Source isolation

- `D:\Projects\Something-v2` is read-only reference material. Inspect source, tests, documentation, and assets only. Never edit, format, delete, move, or generate files there, and never modify its persistent application data.
- Make project changes only under `D:\Projects\Something-Qt`. Inspect existing files and preserve unrelated user changes before editing.
- Do not initialize, reset, rewrite, or otherwise modify Git state unless the user explicitly authorizes a specific Git operation.

## Command restrictions

- Do not run tests, builds, CMake configure/generate commands, development servers, applications, profilers, or packaging/deployment commands unless the user explicitly authorizes that specific command.
- A general request to implement a feature is not authorization to execute those commands. Continue with source-level inspection and edits, and report what remains unverified.
- Toolchain discovery may inspect environment variables, paths, package metadata, and file version metadata. Do not execute compiler, CMake, Ninja, Qt, test, or application binaries for discovery.

## Product boundaries

- This is an independent C++20 / Qt 6 / Qt Quick project. Keep UI in QML and application behavior in C++ layers.
- Exclude MCP, CLI, REST API, Git Workspaces, Website Search, Utilities, Bookmarks, Notes, and Voice.
- Do not add a local HTTP listener, WebEngine/WebView, Go helper, or voice dependency.
- Keep test-only dependencies behind the opt-in test configuration; never link them into the runtime application target.
- Do not commit machine-specific absolute toolchain paths or user-local CMake presets.

## Planning documents

- `docs/feature-parity.md` defines retained behavior and exclusions.
- `docs/architecture.md` defines the proposed dependency direction.
- `docs/verification.md` defines acceptance criteria; it is not evidence that verification has been run.
- Record toolchain assumptions and inspection limits in `docs/toolchain.md`.
