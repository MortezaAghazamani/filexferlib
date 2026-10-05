# Contributing

Thanks for your interest in filexferlib!

## Bug reports

Please include:

- OS and compiler version
- Steps to reproduce
- Expected vs. actual behavior
- Server/client logs (enable `LogLevel::Debug`)

## Pull requests

- Keep changes focused (one feature per PR)
- Add tests for new behavior
- Ensure `ctest` passes locally
- Follow the existing code style

## Running tests

```bash
cmake -B build -S .
cmake --build build -j
ctest --test-dir build --output-on-failure
