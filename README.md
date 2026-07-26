# dmarp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](../../LICENSE)

dmarp DMOD library module - resolves an IPv4 address to a MAC address on a
directly-connected link (ARP, RFC 826), with a cache so repeated
resolutions of the same destination don't re-send a request every time.
Built on top of [dmnetif](../dmnetif) (`dmnetif_send()`/`_receive()` to
exchange frames) and dmroute (the shared address type).

## Building

This module lives under `lib/dmarp` inside the `dmnet` repository and is
built as part of the parent's CMake configure (the top-level
`CMakeLists.txt` calls `add_subdirectory(lib)`, whose own `CMakeLists.txt`
calls `add_subdirectory(dmarp)` after `add_subdirectory(dmnetif)`) - it is
not built standalone.

```bash
mkdir -p build
cd build
cmake ..
cmake --build . --target dmarp
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

## Usage

```c
#include "dmarp.h"

dmnetif_mac_addr_t mac;
int ret = dmarp_resolve(iface, &gateway_ip, &mac, DMARP_DEFAULT_TIMEOUT_MS);
```

## Documentation

See the `docs/` directory:

- **[dmarp.md](docs/dmarp.md)** - Overview and architecture
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
└── dmarp.dmr
```

LICENSE is shared with the rest of the `dmnet` repository (`../../LICENSE`) -
see `dmarp.dmr` for how it's picked up during packaging.

## Author

Patryk Kubiak

## License

MIT
