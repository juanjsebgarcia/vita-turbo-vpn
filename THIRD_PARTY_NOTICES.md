# Third-party notices

Vita Turbo VPN is MIT-licensed (see [LICENSE](LICENSE)). It includes or links the components below, each under its own licence; the full text of each follows, and the vendored copies sit next to their sources.

| Component | Where | Licence | Used for |
|---|---|---|---|
| OpenBSD tunnel protocol sources | `third_party/openbsd/` | ISC; BSD 3-clause; public domain (per file) | Handshake, sessions and cryptography (blake2s, curve25519, chachapoly, poly1305, siphash, queue.h). |
| CRYPTOGAMS ChaCha20 and Poly1305 for ARM/NEON | `third_party/cryptogams/` | BSD 3-clause (CRYPTOGAMS licence) | Data-path encryption (the NEON assembly). |
| quirc | `third_party/quirc/` | ISC | QR code decoding (profile import). |
| Mozilla root certificates (curl's bundle) | `third_party/ca-bundle/` | MPL-2.0 | Verifying HTTPS for the exit-IP check. |
| Chakra Petch, Barlow Semi Condensed, Martian Mono | `third_party/fonts/` | SIL Open Font License 1.1 | The app's typefaces. |
| Mbed TLS 3.6 | `app/third_party/mbedtls/` | Apache-2.0 | HTTPS for the exit-IP check. |
| miniz | `app/third_party/miniz/` | MIT | Reading .zip profile groups. |
| stb (stb_truetype, stb_image) | `app/third_party/stb/` | MIT or public domain | Text and image drawing in the app. |
| vita2d | `linked from the VitaSDK` | MIT | Drawing in the app. |

## OpenBSD tunnel protocol sources

`third_party/openbsd/` — ISC; BSD 3-clause; public domain (per file)

```
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
```

## CRYPTOGAMS ChaCha20 and Poly1305 for ARM/NEON

`third_party/cryptogams/` — BSD 3-clause (CRYPTOGAMS licence)

```
Copyright (c) 2006, CRYPTOGAMS by <appro@openssl.org>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

      *	Redistributions of source code must retain copyright notices,
	this list of conditions and the following disclaimer.

      *	Redistributions in binary form must reproduce the above
	copyright notice, this list of conditions and the following
	disclaimer in the documentation and/or other materials
	provided with the distribution.

      *	Neither the name of the CRYPTOGAMS nor the names of its
	copyright holder and contributors may be used to endorse or
	promote products derived from this software without specific
	prior written permission.

ALTERNATIVELY, provided that this notice is retained in full, this
product may be distributed under the terms of the GNU General Public
License (GPL), in which case the provisions of the GPL apply INSTEAD OF
those given above.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## quirc

`third_party/quirc/` — ISC

```
quirc -- QR-code recognition library
Copyright (C) 2010-2012 Daniel Beer <dlbeer@gmail.com>

ISC License
===========

Permission to use, copy, modify, and/or distribute this software for
any purpose with or without fee is hereby granted, provided that the
above copyright notice and this permission notice appear in all
copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
```

## Mozilla root certificates (curl's bundle)

`third_party/ca-bundle/` — MPL-2.0

```
# Root certificate bundle

`cacert.pem` is Mozilla's set of trusted root certificates, as extracted and
published by the curl project at https://curl.se/docs/caextract.html. The
app embeds it so its HTTPS checks work on every firmware without iTLS-Enso.
Users can add roots of their own in
`ux0:data/vita-turbo-vpn/extra-ca.pem`.

Licence: the certificate data comes from Mozilla's `certdata.txt`, under the
Mozilla Public License 2.0.

<!-- The update workflow rewrites the three lines below; keep their format. -->
- Source: https://curl.se/ca/cacert.pem
- Mozilla data as of: Tue Oct 20 13:32:40 2026 GMT
- File SHA-256: 796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe

## Updates

When curl publishes a new bundle, replace `cacert.pem`, verify the checksum,
check that every certificate parses, and review the roots added and removed:
a removed root is a service that will stop passing the check. The next app
build carries the new bundle.

Checked on 2026-10-29: the default exit-IP service (via ISRG Root X1/X2),
`api.ipify.org` and `ifconfig.co` (via GTS Root R4) all verify against this
file alone.

The workflow lives in this repository along with this
folder.
```

## Chakra Petch, Barlow Semi Condensed, Martian Mono

`third_party/fonts/` — SIL Open Font License 1.1

```
# Fonts

The app's three typefaces, vendored unmodified. All three
are under the SIL Open Font License 1.1: free to use, embed and
redistribute, including in commercial software, as long as the licence
travels with the fonts and they are not sold on their own. None declares a
Reserved Font Name. Each folder holds the licence text as published with
the font, and the VPK carries the same files under `app0:fonts/`.

| Folder | Files | Used for | Copyright | Licence | Source |
|---|---|---|---|---|---|
| `chakra-petch/` | `ChakraPetch-SemiBold.ttf`, `ChakraPetch-Bold.ttf` | titles, labels (600); status words, tube text (700) | 2018 The Chakra Petch Project Authors | OFL 1.1 (`OFL.txt`) | google/fonts, `ofl/chakrapetch/`, fetched 2026-10-29 |
| `barlow-semi-condensed/` | `BarlowSemiCondensed-Regular.ttf` | body text (400) | 2017 The Barlow Project Authors | OFL 1.1 (`OFL.txt`) | google/fonts, `ofl/barlowsemicondensed/`, fetched 2026-10-29 |
| `martian-mono/` | `MartianMono-StdRg.ttf`, `MartianMono-StdMd.ttf` | addresses (400); values that are the point (500) | 2021 The Martian Mono Project Authors | OFL 1.1 (`OFL.txt`) | evilmartians/mono release v1.1.0, `martian-mono-1.1.0-ttf.zip`, standard width |

Martian Mono comes from its own release rather than Google Fonts because
Google Fonts ships only the variable font, and the app's text renderer
(stb_truetype) draws a variable font at its default instance only. The
"Std" files are the standard width, which is the variable font's default.

Only the weights the design uses are vendored. To add one, take the static
TTF from the same source and add a row here.

SHA-256:

```
45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b  chakra-petch/ChakraPetch-SemiBold.ttf
65fbf76d95651697275e19db4d717c0e95a789ddd3476478b05292104db278a0  chakra-petch/ChakraPetch-Bold.ttf
e8c7242f612b13d026e18a94b63b0fd079e91a3faf009d1b6981dbb5f16d1153  barlow-semi-condensed/BarlowSemiCondensed-Regular.ttf
7ccf33cd01e123d9bf0e0da695440351550c70e572951455a84d22cf55e1d9b4  martian-mono/MartianMono-StdRg.ttf
d4f2b5f9c2a389d1e559ab90fc3b2a121aa3e43838e8129fed229f5c07dc6236  martian-mono/MartianMono-StdMd.ttf
```

## Host-only fallback fonts (not vendored, not shipped)

The screenshot tool (`app/`, `make fallback-fonts`) fetches these
into `app/build-host/fonts/fallback/` to draw scripts the three faces above
lack (Cyrillic, Japanese, Korean, Chinese). They are used for host
screenshots only; the console draws those characters with its own system
fonts, and the VPK does not carry them. `app/tools/fetch_fallback_fonts.sh`
pins each file by SHA-256.

| Files | Copyright | Licence | Source |
|---|---|---|---|
| `NotoSans-Regular.ttf`, `NotoSans-Bold.ttf` (unhinted) | 2022 The Noto Project Authors | OFL 1.1 | notofonts/notofonts.github.io at c1d3718e, `fonts/NotoSans/unhinted/ttf/`; licence from notofonts/latin-greek-cyrillic tag NotoSans-v2.015 |
| `NotoSans{JP,KR,SC,TC}-{Regular,Bold}.otf` (region subsets) | 2014-2021 Adobe (http://www.adobe.com/) | OFL 1.1 | notofonts/noto-cjk tag Sans2.004, `Sans/SubsetOTF/` and `LICENSE` |
```

## Mbed TLS 3.6

`app/third_party/mbedtls/` — Apache-2.0

```
Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```

## miniz

`app/third_party/miniz/` — MIT

```
Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## stb (stb_truetype, stb_image)

`app/third_party/stb/` — MIT or public domain

```
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## vita2d

`linked from the VitaSDK` — MIT

```
The MIT License (MIT)

Copyright (c) 2015 Sergi Granell (xerpi), xerpi.g.12@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
