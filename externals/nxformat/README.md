# NXFormat reader

Header-only reader for NXFormat (`.nx`) archives, used by the NX loader
(`src/core/loader/nx.cpp`). Nemyu builds only this reader: nxformat's converter library, which
Lichii downloads prebuilt in `CMakeModules/NxFormat.cmake`, is not part of the build.

- Upstream: https://github.com/fab918/nxformat by fab918
- Version: tag `V1.0.1`, commit `7534937f9d347d69443fa96958a5162f00ea856a` (updated with Lichii #1078 by fab918)
- License: MIT (`LICENSE`)

The headers below were downloaded from the tag on 2026-11-09 and match Lichii's copies.

| File | Upstream path | SHA-256 |
|---|---|---|
| `nx_reader.hpp` | `dist/nx_reader.hpp` | `5dcd05cd38a5599fadc420879f5b62656d32af45128fd824ea16a236cf366ea8` |
| `nx_session.hpp` | `dist/nx_session.hpp` | `ea27c3851b682da8780bc4e9e07e9522c77f21870ecb6cc05834b0ac31f7f3e9` |
| `nx_disk_source.hpp` | `dist/nx_disk_source.hpp` | `35ce56e0c1316b55353d56910751869befccced0236502ec9743629ff1fa23f2` |
| `LICENSE` | `LICENSE` | `c803c9420a16a056c0ed7459c30c804ebe35ddee989f068ef6db2031ca24bf1a` |

No local patches.

Lichii's prebuilt converter archives are not used here and were not checked for 1.0.1.
