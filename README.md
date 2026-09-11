# Rufus++

Rufus++ is a cross-platform boot-media utility for Windows, Linux, and macOS.
It combines a portable C++17 core with a Qt 6 interface and native device
backends for each operating system.

It can analyze, create, format, verify, and capture bootable media, including
Windows installation and Windows To Go media, persistent Linux live media,
raw and compressed disk images, and native macOS installer applications.

> [!WARNING]
> This project is a development prototype, not a released replacement for
> Rufus. Its destructive operations have not completed platform-specific
> hardware qualification. Do not use it with media containing important data.

This is an unofficial project derived from the GPL-licensed
[Rufus project](https://github.com/pbatard/rufus). Pete Batard and Akeo
Consulting do not publish or support this branch.

## Build and test

You will need:

- CMake 3.24 or later
- A C++17 compiler
- Qt 6.5 or later with Widgets and Network
- Development packages for zlib, libbz2, liblzma, and libzstd

Configure, build, and test the Qt application:

```console
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

If Qt is installed in a non-system prefix:

```console
cmake --preset dev -DCMAKE_PREFIX_PATH=/path/to/Qt
```

To build only the portable core:

```console
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

The resulting Qt executable is placed under `build/dev/src/qt`. On macOS it is
the `Rufus++.app` bundle.

An unsigned macOS development build can inspect media but cannot write to a
physical disk. Write-enabled builds require a matching Apple Team ID and code
signing identity for the app and privileged helper; see the platform
documentation below.

## Documentation

- [Features and runtime requirements](docs/features.md)
- [Architecture, safety model, and platform backends](docs/cross-platform.md)

## License and attribution

This project is a new cross-platform reimplementation inspired by Rufus. The
reimplementation was developed with assistance from OpenAI’s GPT-5.6 Sol model
using the Extra High reasoning setting. AI-assisted development does not
replace independent code review, security assessment, or field testing on
supported platforms and physical removable media.

The project is distributed under the GNU General Public License version 3 or,
at your option, any later version. See [LICENSE.txt](LICENSE.txt).

Rufus and its original source code are copyright Pete Batard and contributors.
New cross-platform code is copyright its respective contributors. Third-party
assets retain their original licences and provenance under `res/`.
