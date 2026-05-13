# SagePkg

The official package repository for [SageLang](https://github.com/Night-Traders-Dev/SageLang).

This repository hosts packages that can be installed using the `sagepkg` tool included with SageLang v3.4.5 and above.

## Structure

Packages are organized by name and architecture to ensure compatibility:

```text
/
├── packages.json              # Global index of available packages
└── packages/
    └── <PackageName>/
        ├── metadata.json      # Package-specific metadata and file list
        ├── x86_64/            # Binary/Source files for x86_64
        └── aarch64/           # Binary/Source files for ARM64
```

## How to use SagePkg

The `sagepkg` tool is the easiest way to interact with this repository.

### 1. Update the local index
Sync your local package list with this repository:
```bash
sagepkg update
```

### 2. List available packages
View what's available for your system:
```bash
sagepkg list
```

### 3. Install a package
Install a package (it will automatically detect your architecture):
```bash
sagepkg install SageFetch
```

### 4. View installed packages
```bash
sagepkg installed
```

## Contributing

To add a new package to this repository:
1. Fork the repository.
2. Add your package files following the directory structure above.
3. Update `packages.json` to include your new package.
4. Submit a Pull Request.

---
© 2026 Night Traders Dev
