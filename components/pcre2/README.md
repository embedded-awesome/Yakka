# PCRE2 component

Vendored upstream release: [PCRE2 10.49](https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.49).
The upstream sources and license are unmodified in `pcre2-10.49/`.

Archive: `pcre2-10.49.tar.gz`
SHA-256: `929f0b20e62879252a15886b06c89f1edef61a363cbd5826fb041080a5e557ae`

`pcre2.yakka` builds the static 8-bit library with Unicode support and without JIT.
It needs no system PCRE2 installation or configure/CMake step. The files in
`generated/` are copies of upstream's portable hand-build defaults:

- `src/config.h.generic` -> `generated/config.h`
- `src/pcre2.h.generic` -> `generated/pcre2.h`
- `src/pcre2_chartables.c.dist` -> `generated/pcre2_chartables.c`

Feature defines are set in the component rather than modifying these copies.
The POSIX wrapper, command-line programs, and upstream tests are not compiled.
