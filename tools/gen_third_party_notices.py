#!/usr/bin/env python3
"""gen_third_party_notices.py - writes THIRD_PARTY_NOTICES.md from the
vendored licence files (run from the repository root after a change in
third_party/ or app/third_party/)."""
import os

def rd(p):
    return open(p, encoding='utf-8', errors='replace').read().strip()

COMPONENTS = [
    ("OpenBSD tunnel protocol sources", "third_party/openbsd/", "ISC; BSD 3-clause; public domain (per file)",
     "Handshake, sessions and cryptography (blake2s, curve25519, chachapoly, poly1305, siphash, queue.h).",
     "third_party/openbsd/LICENSES.md"),
    ("CRYPTOGAMS ChaCha20 and Poly1305 for ARM/NEON", "third_party/cryptogams/", "BSD 3-clause (CRYPTOGAMS licence)",
     "Data-path encryption (the NEON assembly).", "third_party/cryptogams/LICENSE"),
    ("quirc", "third_party/quirc/", "ISC", "QR code decoding (profile import).", "third_party/quirc/LICENSE"),
    ("Mozilla root certificates (curl's bundle)", "third_party/ca-bundle/", "MPL-2.0",
     "Verifying HTTPS for the exit-IP check.", "third_party/ca-bundle/README.md"),
    ("Chakra Petch, Barlow Semi Condensed, Martian Mono", "third_party/fonts/", "SIL Open Font License 1.1",
     "The app's typefaces.", "third_party/fonts/LICENSES.md"),
    ("Mbed TLS 3.6", "app/third_party/mbedtls/", "Apache-2.0", "HTTPS for the exit-IP check.",
     "app/third_party/mbedtls/LICENSE"),
    ("miniz", "app/third_party/miniz/", "MIT", "Reading .zip profile groups.", "app/third_party/miniz/LICENSE"),
    ("stb (stb_truetype, stb_image)", "app/third_party/stb/", "MIT or public domain",
     "Text and image drawing in the app.", "app/third_party/stb/LICENSE"),
    ("vita2d", "linked from the VitaSDK", "MIT", "Drawing in the app.", "third_party/vita2d-LICENSE.txt"),
]

out = ['# Third-party notices', '',
       'Vita Turbo VPN is MIT-licensed (see [LICENSE](LICENSE)). It includes or links the components below, '
       'each under its own licence; the full text of each follows, and the vendored copies sit next to their sources.',
       '', '| Component | Where | Licence | Used for |', '|---|---|---|---|']
for name, where, lic, used, _ in COMPONENTS:
    out.append(f'| {name} | `{where}` | {lic} | {used} |')
out.append('')
for name, where, lic, _, path in COMPONENTS:
    out += [f'## {name}', '', f'`{where}` — {lic}', '', '```', rd(path), '```', '']
open('THIRD_PARTY_NOTICES.md', 'w', encoding='utf-8').write('\n'.join(out))
print('THIRD_PARTY_NOTICES.md written')
