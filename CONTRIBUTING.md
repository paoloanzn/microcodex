# Contributing

Thank you for your interest in contributing to MicroCodex.

## Reporting issues

Before opening an issue, please search the existing issues to avoid duplicates. When reporting a bug, include:

- A clear description of the problem
- Steps to reproduce it
- The expected and actual behavior
- Your operating system and compiler version
- Any relevant logs or error messages

Never include credentials, access tokens, or other sensitive information.

## Development setup

MicroCodex requires a C++23 compiler, `make`, libcurl development files, and OpenSSL development files on Linux.

```sh
git clone --recurse-submodules https://github.com/paoloanzn/microcodex.git
cd microcodex
make
```

The executable is written to `build/microcodex`.

## Making changes

1. Fork the repository and create a branch from the default branch.
2. Keep changes focused and avoid unrelated refactoring.
3. Follow the style of the surrounding code.
4. Add or update tests when behavior changes.
5. Build the project and run the test suite:

   ```sh
   make
   make test
   ```

## Pull requests

When opening a pull request:

- Use a clear, descriptive title.
- Explain what changed and why.
- Link any related issues.
- Describe how the change was tested.
- Keep each pull request limited to one logical change.
- Ensure the project builds without warnings and all tests pass.

> [!NOTE]
> Please do not submit AI-generated pull request descriptions or diffs without reviewing them carefully. Contributors are responsible for understanding and validating everything they submit.

By submitting a contribution, you agree that it will be licensed under the repository's [Apache License 2.0](LICENSE).
