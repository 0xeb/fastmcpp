# Git Hooks

This directory contains git hooks for the fastmcpp project.

## Installation

Run one of the following commands to enable the hooks:

```bash
# Option 1: Configure git to use this directory for hooks
git config core.hooksPath .githooks

# Option 2: Symlink (Linux/macOS)
ln -sf ../../.githooks/pre-commit .git/hooks/pre-commit
```

## Available Hooks

### pre-commit

Automatically formats staged C++ files with clang-format before commit.

- Finds clang-format (prefers versioned like clang-format-19)
- Formats only staged `.cpp`, `.hpp`, `.h`, `.c` files
- Re-stages formatted files automatically

This ensures all committed code follows the project's formatting style.
