# Contributing

LASO is an early native framework. Keep mechanisms generic, dependencies restrained
and execution behavior explicit. Do not add deployment-specific data or secrets.

Use Linux, C++20 and the distribution dependencies in the README. Configure Debug
with Ninja, build, and run `ctest --test-dir build --output-on-failure` before
submitting a change. Test additions should cover behavior and recovery boundaries,
not duplicate implementation details. All tests must work offline after packages
are installed.

Format C and C++ sources with clang-format **18**. Run clang-tidy against the
compilation database:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
find include src apps tests plugin_sdk -type f \
  \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.c' \) \
  -print0 | xargs -0 clang-format-18 --dry-run --Werror
find src apps -name '*.cpp' -print0 | xargs -0 -n 1 clang-tidy-18 -p build
```

Do not change the C ABI layout silently. Document ownership and cancellation for
extensions. Never permit exceptions or STL objects across the plugin boundary.
Before modifying checkpoint behavior, test a real process restart during approval.
Use separate directories for GCC, Clang and sanitizer builds.

Include a clear problem statement, behavior change and actual validation results
in pull requests. Mark checks that were not executed as PENDING. Contributions
are offered under the repository's Apache License 2.0 terms.
