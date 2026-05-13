# SageShell

An interactive, [fish](https://fishshell.com/)-like shell clone written in [SageLang](https://github.com/Night-Traders-Dev/SageLang).

## Features
- **Interactive Prompt**: Beautiful colored prompt displaying `user@host` and the current working directory.
- **Built-in Commands**: Supports internal commands like `cd`, `clear`, `help`, and `exit`.
- **Process Execution**: Passes unknown commands to the system shell for execution.
- **State Management**: Maintains its own internal `CWD` state since Sage processes don't inherently change their parent's environment.

## Installation
Use the `sagepkg` tool included with SageLang v3.4.5+:
```bash
sagepkg install SageShell
```

## Usage
Once installed, run SageShell:
```bash
sage ~/.sagepkg/pkgs/SageShell/main.sage
```

From inside the shell, type `help` to see available commands or run standard system binaries like `ls`, `cat`, etc.

---
© 2026 Night Traders Dev
