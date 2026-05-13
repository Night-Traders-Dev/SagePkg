.PHONY: install clean help

help:
	@echo "SagePkg Makefile"
	@echo "Available targets:"
	@echo "  install   - Install sagepkg to ~/.sagepkg/bin and update PATH"
	@echo "  clean     - Remove any local build artifacts (currently none)"

install:
	@bash install.sh

clean:
	@echo "Nothing to clean."
