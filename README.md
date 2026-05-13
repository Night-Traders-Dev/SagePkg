# SagePkg

The official package repository for [SageLang](https://github.com/Night-Traders-Dev/SageLang).

## Automated Setup
As of SageLang v3.4.5, running `sagepkg update` will automatically detect your shell (Bash, Zsh, or Fish) and add the Sage binary directory (`~/.sagepkg/bin`) to your `PATH` if it's not already there.

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