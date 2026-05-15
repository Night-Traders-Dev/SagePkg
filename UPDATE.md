# SagePkg Updates

## [1.4.0] - 2026-05-15
### Added
- **SagePkg**: Bundled `json.sage` dependency to ensure cross-platform compatibility.
- **SageShell**: Fixed missing imports and refactored to use standard `sys` and `io` modules.
- **SageShell**: Fixed SageLang REPL support by ensuring correct terminal state and execution.
- **SageFetch**: Fixed missing imports and refactored to use standard `sys` and `io` modules.
- **SageFetch**: Improved OS, Kernel, and Hardware detection logic.

## [1.3.0] - 2026-05-15
### Added
- **SagePkg**: Support for `riscv64` architecture.
- **SagePkg**: Improved reliability of architecture and path detection.
- **SagePkg**: Dynamic installation dates for packages.
- **SagePkg**: Input validation for package names to prevent command injection.
- **SagePkg**: Better error reporting for network and compilation failures.

## [1.2.0] - 2026-05-13
### Added
- **SagePkg**: New `build` command to force compilation from source.
- **SagePkg**: New `remove` command to uninstall packages.
- **SagePkg**: Automatic fallback to building from source if binary is missing during `install`.

## [1.1.0] - 2026-05-13
### Added
- **SageShell**: Support for keyboard key combos (Ctrl+L, Ctrl+D, Ctrl+C).
- **SageShell**: Persistent command history via Up/Down arrows.
- **SageShell**: New prompt using the Sage emoji 🌿.
- **SagePkg**: Support for universal source code directory (`universal/`).
- **SagePkg**: Architecture-specific binaries for `aarch64`.

### Changed
- Refactored package structure to separate source (`universal/`) and binaries.
- Updated metadata versioning for all packages.
- Optimized SageFetch for faster execution.

### Fixed
- Improved robustness of terminal output handling in SageShell.
- Fixed character deletion and cursor movement in custom input loop.
