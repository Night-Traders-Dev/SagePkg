# SagePkg

The official package repository for [SageLang](https://github.com/Night-Traders-Dev/SageLang).

## Installation

To install the `sagepkg` tool itself:

```bash
# Clone the repository
git clone https://github.com/Night-Traders-Dev/SagePkg
cd SagePkg

# Install using Makefile
make install
```

This will install `sagepkg` to `~/.sagepkg/bin` and update your shell's `PATH`.

## How to use SagePkg

### 1. Sync and Initialize
```bash
sagepkg update
```
This downloads the latest package list and ensures your environment is ready.

### 2. Install a package
```bash
sagepkg install SageFetch
```

### 3. Run by name
Once installed, you can call packages directly:
```bash
SageFetch
```

## Available Packages
- **SageFetch**: A colorful Neofetch clone.
- **SageShell**: An interactive fish-like shell.

---
© 2026 Night Traders Dev