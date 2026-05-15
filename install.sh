#!/bin/bash
set -e

SAGEPKG_HOME="$HOME/.sagepkg"
BIN_DIR="$SAGEPKG_HOME/bin"
PKGS_DIR="$SAGEPKG_HOME/pkgs"

echo "Installing SagePkg..."

# Create directories
mkdir -p "$BIN_DIR"
mkdir -p "$PKGS_DIR"

# Copy sagepkg.sage and dependencies to bin
cp sagepkg.sage "$BIN_DIR/sagepkg"
cp json.sage "$BIN_DIR/json.sage"
chmod +x "$BIN_DIR/sagepkg"

echo "SagePkg script installed to $BIN_DIR/sagepkg"

# Add to PATH
SHELL_NAME=$(basename "$SHELL")
CONFIG_FILE=""

case "$SHELL_NAME" in
    bash)
        CONFIG_FILE="$HOME/.bashrc"
        ;;
    zsh)
        CONFIG_FILE="$HOME/.zshrc"
        ;;
    fish)
        CONFIG_FILE="$HOME/.config/fish/config.fish"
        ;;
    *)
        echo "Unknown shell: $SHELL_NAME. Please add $BIN_DIR to your PATH manually."
        ;;
esac

if [ -n "$CONFIG_FILE" ]; then
    if ! grep -q "$BIN_DIR" "$CONFIG_FILE"; then
        echo "Adding $BIN_DIR to PATH in $CONFIG_FILE..."
        if [ "$SHELL_NAME" == "fish" ]; then
            echo -e "\n# SagePkg PATH\nset -gx PATH \"$BIN_DIR\" \$PATH" >> "$CONFIG_FILE"
        else
            echo -e "\n# SagePkg PATH\nexport PATH=\"$BIN_DIR:\$PATH\"" >> "$CONFIG_FILE"
        fi
        echo "Success: PATH updated. Please restart your shell or run 'source $CONFIG_FILE'."
    else
        echo "PATH already contains $BIN_DIR in $CONFIG_FILE."
    fi
fi

echo "Done!"
