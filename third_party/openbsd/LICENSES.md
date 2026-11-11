# OpenBSD sources: licences

Vendored unmodified from the OpenBSD source tree, through its GitHub mirror:

- Repository: <https://github.com/openbsd/src>
- Commit: `eeba8a4a486f147098b87d8bc0796a524d7b41f2` (2026-10-29, "Remove
  unused interrupt-related routines.")
- Fetched from `https://raw.githubusercontent.com/openbsd/src/<commit>/<path>`
  on 2026-10-29. Paths below are relative to this directory and match the
  OpenBSD tree.

Every file's licence header was read in full, and each file was searched for
any further copyright, licence or permission text past the header. None was
found.

## Summary

| File | Licence | Verdict |
|---|---|---|
| `sys/net/wg_noise.c`, `wg_noise.h` | ISC | permissive |
| `sys/net/wg_cookie.c`, `wg_cookie.h` | ISC | permissive |
| `sys/crypto/blake2s.c`, `blake2s.h` | ISC | permissive |
| `sys/crypto/curve25519.c`, `curve25519.h` | ISC | permissive |
| `sys/crypto/chachapoly.c`, `chachapoly.h` | ISC | permissive |
| `sys/crypto/chacha_private.h` | public domain | permissive |
| `sys/crypto/poly1305.c`, `poly1305.h` | public domain | permissive |
| `sys/crypto/siphash.c`, `siphash.h` | BSD 3-clause | permissive |
| `sys/sys/queue.h` | BSD 3-clause | permissive |
| `sys/sys/_null.h` | public domain | permissive |

**Verdict: every file allows a permissive release.** None is GPL or
carries any copyleft term. Nothing was rejected.

Obligations for a release:
- ISC files: keep the copyright and permission notice in all copies. Put
  the notices in the release's third-party notices so binary copies carry
  them too.
- BSD 3-clause files (`siphash.*`, `queue.h`): source copies keep the notice;
  binary distributions must reproduce the notice, the conditions and the
  disclaimer in their documentation; the authors' names (Andre Oppermann; the
  University of California) may not be used to endorse or promote the
  product.
- Public-domain files: nothing required. Credit is still given.

## Licence texts

The ISC text used by the ISC files, word for word (only the copyright lines
differ between files):

> Permission to use, copy, modify, and distribute this software for any
> purpose with or without fee is hereby granted, provided that the above
> copyright notice and this permission notice appear in all copies.
>
> THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
> WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
> ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
> WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
> ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
> OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

The BSD 3-clause operative text of `siphash.c` and `siphash.h`:

> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions
> are met:
> 1. Redistributions of source code must retain the above copyright
>    notice, this list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright
>    notice, this list of conditions and the following disclaimer in the
>    documentation and/or other materials provided with the distribution.
> 3. The name of the author may not be used to endorse or promote
>    products derived from this software without specific prior written
>    permission.

followed by the usual "AS IS ... SUCH DAMAGE" disclaimer. `queue.h` has the
same three conditions, with clause 3 reading "Neither the name of the
University nor the names of its contributors may be used to endorse or
promote products derived from this software without specific prior written
permission."

## Per file

### `sys/net/wg_noise.c`
- SHA-256 `c5855b6e0b03dc9cc324dbe0c726fa6e12170c31ca4d8534f0383bc3e62afc30`
- RCS id: `wg_noise.c,v 1.8 2025/10/27 17:36:33 mvs`
- Copyright: (C) 2015-2020 Jason A. Donenfeld; (C) 2019-2020 Matt Dunwoodie
- Licence: ISC ("Permission to use, copy, modify, and distribute this
  software for any purpose with or without fee is hereby granted, provided
  that the above copyright notice and this permission notice appear in all
  copies.")
- Verdict: permissive

### `sys/net/wg_noise.h`
- SHA-256 `67d0ca04de4ee344bad78d695b055da922d36a55b6257a6739237a3725fc310a`
- RCS id: `wg_noise.h,v 1.3 2024/03/05 17:48:01 mvs`
- Copyright: (C) 2015-2020 Jason A. Donenfeld; (C) 2019-2020 Matt Dunwoodie
- Licence: ISC (text as above)
- Verdict: permissive

### `sys/net/wg_cookie.c`
- SHA-256 `de73c0264853e0e95bb52e280ffb1bf9a6e05969828028ddd6f47bb7d2f12da7`
- RCS id: `wg_cookie.c,v 1.5 2023/08/18 08:11:47 jsg`
- Copyright: (C) 2015-2020 Jason A. Donenfeld; (C) 2019-2020 Matt Dunwoodie
- Licence: ISC (text as above)
- Verdict: permissive

### `sys/net/wg_cookie.h`
- SHA-256 `f6bd83b0f8d43c744ad497213b6f53cee422d7a51e5faf8b07238991ea23e0d7`
- RCS id: `wg_cookie.h,v 1.2 2020/12/09 05:53:33 tb`
- Copyright: (C) 2015-2020 Jason A. Donenfeld; (C) 2019-2020 Matt Dunwoodie
- Licence: ISC (text as above)
- Verdict: permissive

### `sys/crypto/blake2s.c`
- SHA-256 `b31ce4398f66dc8b5c8f87bda29093d900019f0c08c49b80ae244229e9d2d74e`
- RCS id: `blake2s.c,v 1.3 2023/02/03 18:31:16 miod`
- Copyright: (C) 2012 Samuel Neves; (C) 2015-2020 Jason A. Donenfeld
- Licence: ISC (text as above). The header also says: "This is an
  implementation of the BLAKE2s hash and PRF functions." No other terms.
- Verdict: permissive. (The several copyright lines here all sit under the
  one ISC grant.)

### `sys/crypto/blake2s.h`
- SHA-256 `7b3e663a85b2fd3b414e3d209dfe11b85bd74f0b2cd7705603989999326c2314`
- RCS id: `blake2s.h,v 1.3 2023/02/03 18:31:16 miod`
- Copyright: (C) 2015-2020 Jason A. Donenfeld; (C) 2019-2020 Matt Dunwoodie
- Licence: ISC (text as above)
- Verdict: permissive

### `sys/crypto/curve25519.c`
- SHA-256 `c7d3e7bff0f4d553422f7797cc52a53be5d0895753f63e8b28ff77c172720185`
- RCS id: `curve25519.c,v 1.2 2020/07/22 13:54:30 tobhe`
- Copyright: (C) 2018-2020 Jason A. Donenfeld; (C) 2015-2016 The fiat-crypto
  Authors
- Licence: ISC (text as above). The header notes the code is a
  machine-generated, formally verified implementation from fiat-crypto
  (<https://github.com/mit-plv/fiat-crypto>), "tweaked to be suitable for use
  in the kernel". The fiat-crypto project itself is distributed under the
  MIT, Apache-2.0 and BSD-1-Clause licences at the user's pick (its README,
  checked 2026-10-29), all permissive, so the ISC grant here is consistent
  with its origin.
- Verdict: permissive. (This file's two copyright lines both sit under the
  one ISC grant.)

### `sys/crypto/curve25519.h`
- SHA-256 `c9fdc3d5438ffbc6f21bc4a63ab8d73f77bfb636755c2c60378fd13ef5caec91`
- RCS id: `curve25519.h,v 1.2 2020/07/22 13:54:30 tobhe`
- Copyright: (C) 2019-2020 Matt Dunwoodie
- Licence: ISC (text as above)
- Verdict: permissive

### `sys/crypto/chachapoly.c`
- SHA-256 `87e49a2f75750f9e4d82530ae6cf4d2e0a0e5d5a6440a58cb3aa6ffaca553c13`
- RCS id: `chachapoly.c,v 1.6 2020/07/22 13:54:30 tobhe`
- Copyright: (c) 2015 Mike Belopuhov
- Licence: ISC (text as above)
- Verdict: permissive

### `sys/crypto/chachapoly.h`
- SHA-256 `b9f78c379ef6bfc8d3a2c48d0b0e3b3d46d49deaae535d5ddf95df81ff65998d`
- RCS id: `chachapoly.h,v 1.4 2020/07/22 13:54:30 tobhe`
- Copyright: (c) 2015 Mike Belopuhov
- Licence: ISC (text as above)
- Verdict: permissive

### `sys/crypto/chacha_private.h`
- SHA-256 `fd0e627d27513adb18a1461ca54722ccd84893450e376ec451c7ff1c261b33d0`
- RCS id: `chacha_private.h,v 1.4 2020/07/22 13:54:30 tobhe`
- Author: D. J. Bernstein ("chacha-merged.c version 20080118")
- Licence: public domain (header, in full: "chacha-merged.c version 20080118
  / D. J. Bernstein / Public domain.")
- Verdict: permissive

### `sys/crypto/poly1305.c`
- SHA-256 `42be81be4f8f31dd2b74632fad3ff0d72b13b65fb728aa6014c6fa8cb96f1ef9`
- RCS id: `poly1305.c,v 1.2 2020/07/22 13:54:30 tobhe`
- Author: Andrew Moon (poly1305-donna)
- Licence: public domain ("Public Domain poly1305 from Andrew Moon / Based on
  poly1305-donna.c, poly1305-donna-32.h and poly1305-donna.h from:
  https://github.com/floodyberry/poly1305-donna")
- Verdict: permissive

### `sys/crypto/poly1305.h`
- SHA-256 `25353f06bd93507f4252ffb73e840c74dfc4e47288d6dc54ee7bb88e1182d48c`
- RCS id: `poly1305.h,v 1.2 2020/07/22 13:54:30 tobhe`
- Author: Andrew Moon
- Licence: public domain ("Public Domain poly1305 from Andrew Moon")
- Verdict: permissive

### `sys/crypto/siphash.c`
- SHA-256 `fcbf8d32ab452706a7e0987e8ddea68943edba59a64bdb1983db278bd2c631eb`
- RCS id: `siphash.c,v 1.5 2018/01/05 19:05:09 mikeb`
- Copyright: (c) 2013 Andre Oppermann. All rights reserved.
- Licence: BSD 3-clause (text above; clause 3 is "The name of the author may
  not be used to endorse or promote products derived from this software
  without specific prior written permission.")
- Verdict: permissive. Note: sometimes listed as BSD-2; it has three
  clauses.

### `sys/crypto/siphash.h`
- SHA-256 `c52f7a73861c8e206bcdc61e5325054e0f2074322b1daa27a2e077b7fc759df5`
- RCS id: `siphash.h,v 1.6 2024/09/04 07:54:52 mglocker`
- Copyright: (c) 2013 Andre Oppermann. All rights reserved.
- Licence: BSD 3-clause (as `siphash.c`)
- Verdict: permissive

### `sys/sys/queue.h`
Vendored because `wg_noise.h` and `wg_cookie.h` use its `SLIST`/`LIST`
macros, and the Vita SDK's own `sys/queue.h` is a different copy.
- SHA-256 `a2abb97bd570e63c6f547174d2557d5d8c640bd669c61efdfd20d707e7a8dd13`
- RCS id: `queue.h,v 1.47 2026/06/12 01:04:42 millert` (from NetBSD 1.11)
- Copyright: (c) 1991, 1993 The Regents of the University of California.
  All rights reserved.
- Licence: BSD 3-clause ("Redistribution and use in source and binary forms,
  with or without modification, are permitted provided that the following
  conditions are met: ..."; clause 3 is the "Neither the name of the
  University nor the names of its contributors" form)
- Verdict: permissive

### `sys/sys/_null.h`
Included by `queue.h`.
- SHA-256 `4814ceea49a3f80f3ec94575bffe878782f512a3882774703b9ae35695f825c6`
- RCS id: `_null.h,v 1.2 2016/09/09 22:07:58 millert`
- Author: Todd C. Miller ("Written by Todd C. Miller, September 9, 2016")
- Licence: public domain ("Public domain.")
- Verdict: permissive

## Not vendored

`sys/net/if_wg.c` (ISC, same authors; `if_wg.c,v 1.50 2026/09/20`) was read
at the same commit for its timer constants and wire formats, which
`core/include/tn_core.h` restates with attribution. No code from it is
copied.
