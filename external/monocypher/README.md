# Monocypher, vendored

Ed25519 signature checking for the licence code in `src/licence/`. Nothing else
in the plugins uses it.

| | |
| --- | --- |
| Upstream | https://github.com/LoupVaillant/Monocypher |
| Version | 4.0.2 (`0d85f98c9d9b0227e42cf795cb527dff372b40a4`) |
| Files | `src/monocypher.{c,h}`, `src/optional/monocypher-ed25519.{c,h}`, `LICENCE.md` — verbatim |
| Licence | BSD-2-Clause or CC0, at the recipient's choice (see `LICENCE.md`) |

Vendored as plain files rather than a submodule because it is four files that
never need to move, and compiled straight into each plugin like the FFGL SDK so
there is no library for a customer to install.

The vendor's C++ SDK recommends libsodium for plugins. It was not used because
it has no CMake build: shipping it would mean three different ways of getting
it (apt, prebuilt MSVC zips, and building from source twice and `lipo`-ing for
the universal macOS bundle), each downloaded at CI time. Monocypher compiles
from source on every toolchain this project already has, needs no
initialisation call, was audited by Cure53 in 2020, and its answers are checked
against the licence service's own signed vectors in `tests/test_licence.cpp`.
