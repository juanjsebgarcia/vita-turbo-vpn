# CRYPTOGAMS: ChaCha20 and Poly1305 for ARMv7

Vendored unmodified from the CRYPTOGAMS repository by Andy Polyakov:

- Repository: <https://github.com/dot-asm/cryptogams>
- Commit: `4acc4a308eab6049d1cf1c7ba61b353a36ce0e2d` (2026-10-20)
- Fetched with `git clone --depth 1` on 2026-10-30.

| File | What | Licence |
|---|---|---|
| `LICENSE` | the CRYPTOGAMS licence, verbatim | - |
| `arm/chacha-armv4.pl` | ChaCha20, ARMv4 integer + NEON (3xNEON+1xIALU) | CRYPTOGAMS licence (`LICENSE`): BSD 3-clause style, or the GPL at the distributor's choice |
| `arm/poly1305-armv4.pl` | Poly1305, ARMv4 integer + NEON | `SPDX-License-Identifier: GPL-1.0+ OR BSD-3-Clause` |
| `arm/arm-xlate.pl` | the perlasm-to-assembler translator the two need | CRYPTOGAMS licence (`LICENSE`) |
| `arm/arm_arch.h` | ARM feature macros (unused by our `__KERNEL__` build) | CRYPTOGAMS licence (`LICENSE`) |
| `generated/*.S` | `generate.sh` output, checked in so building needs no perl | as their `.pl` |
| `generate.sh` | ours: regenerates `generated/` | project |

## Licence review

`LICENSE` was read in full. It is a two-way licence: a BSD 3-clause style
grant (keep the copyright notice, the conditions and the disclaimer in
source; reproduce them in the documentation of binary distributions; do not
use the CRYPTOGAMS name or its contributors' names to endorse products), or
**alternatively** the GPL. `poly1305-armv4.pl` states the same choice as
`GPL-1.0+ OR BSD-3-Clause`. `chacha-armv4.pl`, `arm-xlate.pl` and
`arm_arch.h` carry no other licence text, so `LICENSE` applies.

**This project takes the BSD 3-clause option** for all of them. It is
permissive and imposes nothing on the project's own licence, so the
GPL alternative is not used.

Obligations for a release:
- Source copies keep `LICENSE` and the notices in the `.pl` files (and the
  header line `generate.sh` writes into `generated/*.S`).
- Binary copies (the `.skprx`) reproduce the copyright notice
  ("Copyright (c) 2006, CRYPTOGAMS by <appro@openssl.org>"), the three
  conditions and the disclaimer in the release's third-party notices,
  beside OpenBSD's (`../openbsd/LICENSES.md`).
- The CRYPTOGAMS name is not used to promote the product.

Nothing here is GPL-only.

## How the core uses it

`generate.sh` runs each `.pl` with flavour `linux32` (GNU assembler syntax,
accepted by VitaSDK's binutils and by any ARM GCC). The core never edits the
output: `core/crypto/cg_chacha.S` and `core/crypto/cg_poly1305.S` define
the generator's own `__KERNEL__` and `__LINUX_ARM_ARCH__=7` switches, rename
the entry points to `tncg_*`, and `#include` the generated file. In that
configuration:

- there is no run-time CPU probe in the assembly (`OPENSSL_armcap_P`), so
  the code has no PC-relative reference into another segment, and the module
  carries only ordinary relocations (`R_ARM_ABS32`, `THM_CALL`, `THM_JUMP24`,
  `MOVW/MOVT`);
- the integer and NEON entry points are both exported, and the C glue
  (`core/crypto/chachapoly_cg.c`) chooses per message.

Regenerate with `./generate.sh` (needs perl only) after updating `arm/`.
