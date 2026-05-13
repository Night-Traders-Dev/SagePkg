# SageFetch

A [Neofetch](https://github.com/dylanaraps/neofetch) clone written entirely in [SageLang](https://github.com/Night-Traders-Dev/SageLang).

## Features
- **System Info**: Displays OS, Kernel, Uptime, Shell, CPU, and Memory usage.
- **Fast**: Parsed directly from `/proc` and `/etc/os-release`.
- **Aesthetic**: Includes a custom Sage logo built with ANSI color codes.

## Installation
Use the `sagepkg` tool included with SageLang v3.4.5+:
```bash
sagepkg install SageFetch
```

## Usage
Once installed, you can run SageFetch using the Sage interpreter:
```bash
sage ~/.sagepkg/pkgs/SageFetch/main.sage
```

---
© 2026 Night Traders Dev
