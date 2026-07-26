# dmarp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmarp DMOD library module.

## Description

TODO: describe what this module does.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Usage

This library module provides functions that can be used by other modules:

```c
#include "dmarp.h"
```

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmarp`.
## Project Structure

```
dmarp/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmarp.h
├── src/
│   └── dmarp.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmarp_test.c
├── CMakeLists.txt
├── Makefile
├── dmarp.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
