# The single include

Two files, each needing nothing but the standard library (and pthreads to link):

- `ts_moveables.hpp`: the whole library, `TSMoveables/ts_moveables.hpp` with every header and the snicholls-core
  headers it uses expanded in place, once each. The opt-in headers (TLS, zlib, and the C++20 POSIX parts) are not in it;
  `tooling.conf` lists them with the reason for each.
- `ts_http_server.hpp`: the HTTP server alone, from `TSMoveables/http/server.hpp`.

Copy one into a project, `#include` it and link pthreads.

- **Download them** from the [release](https://github.com/saxonnicholls/ts-moveables/releases) for the version you
  pin. Each release's files are generated from that release's tagged commit and compiled on their own as C++17 before
  they are attached.
- **Build them** for any other commit with `make single-include`, which writes them here (git-ignored), or with
  `python3 third_party/tooling/scripts/single_include.py --out <dir>`.

They are never committed, so they are never stale: every CMake build generates them and compiles them alone
(`ts_moveables_single_include`), the CTest `check.single_include` fails if a header is in neither file nor opted out,
and `make check-amalgamate` builds and runs `tests/amalgamation/drop_in.cpp` against them alone.
