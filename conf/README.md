# vtconf: profile parser

Reads a standard `.conf` profile into the `VtProfile` that the kernel
module takes (`../include/vpnturbo.h`), and provides the helpers the app needs
around profiles: the QR duplicate check, automatic file names and the
subnet-clash warning.

Portable C99. It allocates nothing, does no I/O and uses no locale. Its
only library calls are `memchr`, `memcpy`, `memmove`, `memset` and `strlen`.
It builds unchanged on a desktop and for a Vita user app.

| File | Contents |
|---|---|
| `vtconf.h` | API |
| `vtconf.c` | the parser and `vtc_to_profile` |
| `vtconf_util.c` | messages, duplicate check, file names, subnet checks |
| `tests/test_vtconf.c` | host tests |
| `vita-check.sh` | cross-compile check in the pinned VitaSDK image |

```sh
make test       # host tests with address and undefined-behaviour sanitisers
make coverage   # line and branch coverage (llvm-cov; set PROFDATA and LLVMCOV if not via xcrun)
make vita       # build/vita/libvtconf.a with arm-vita-eabi-gcc, -Wall -Wextra
```

## API

```c
VtcResult r;                         /* about 2 KB; static or on the heap is fine */
VtProfile p;

if (vtc_parse(text, len, &r) != VTC_OK) {
	char msg[160];
	vtc_format_error(&r, msg, sizeof msg);  /* "Line 7: `Jc': key from ..." */
	...
}
for (i = 0; i < r.note_count; i++)          /* ignored lines, for the Profiles screen */
	vtc_format_note(&r.notes[i], msg, sizeof msg);
vtc_to_profile(&r, "home.conf", &p);        /* name: file name, ".conf" removed */
if (!r.endpoint_is_ip)
	p.endpoint_ip = resolve(r.endpoint_host);  /* the app's job */
vtSetProfile(&p);
vtc_wipe(&r, sizeof r);
vtc_wipe(&p, sizeof p);
```

`vtc_parse` takes a length, so the text needs no terminator. Its stack use
is about 2.3 KB, mostly one line buffer, which is wiped before it returns.
On failure `error`, `error_line` (1-based, or 0 for the file as a whole) and
`error_item` (the key or entry concerned) say why, and the key fields are
wiped. An error never quotes a key's value: it names the key.

What a successful `VtcResult` holds besides the `VtProfile` fields:

- `endpoint_host` and `endpoint_port` kept apart; `endpoint_is_ip` and
  `endpoint_ip` when the host is a dotted quad;
- `full_tunnel`: the IPv4 AllowedIPs together cover every address (true for
  `0.0.0.0/0`, and for `0.0.0.0/1, 128.0.0.0/1`);
- `table_off` (`Table = off`; becomes `VT_PROFILE_NO_ROUTES`);
- `check` and `check_url`, from the directive this project adds (below);
- up to 16 `notes`, each a line, a `VTC_N_*` code and the entry, plus a count
  of any beyond that.

Helpers:

| Function | Purpose |
|---|---|
| `vtc_normalise(in, len, out)` | Normal form: CRLF to LF, one trailing newline dropped. `out` may be `in`. Also the input to the `profiles.meta` hash |
| `vtc_same_text(a, alen, b, blen)` | the duplicate check, without a buffer |
| `vtc_qr_file_name(host, exists, ctx, out, cap)` | `host.conf`, then `host-2.conf` ... `host-999.conf` while `exists()` says taken |
| `vtc_subnet_clash(profile, net, prefix, &index)` | warning against the Wi-Fi subnet |
| `vtc_covers_all(list, count)` | whether prefixes cover all of IPv4 |
| `vtc_error_text`, `vtc_note_text` | English text for each code (the app may translate by code) |

## The format

As in the reference tools (`config.c` and the setup script):

- a line ends at `\n`; everything from `#` on is a comment;
- every whitespace byte (space, `\t \n \v \f \r`) is removed, so
  `Allowed IPs = 10.0.0.0 / 8` is `AllowedIPs=10.0.0.0/8`, CRLF files read
  like LF files, and spaces inside a base64 key vanish;
- what is left is `[Interface]`, `[Peer]` or `key=value`, split at the
  first `=`; sections and keys are case-insensitive;
- a line before any section, an unknown section, a line without `=`, and an
  unknown key are all errors;
- `Address`, `DNS` and `AllowedIPs` are comma lists and accumulate when
  repeated; for every other key the last value wins;
- `[Interface]` may appear more than once (keys simply continue).

### Keys

| Key | Accepted | v1 use |
|---|---|---|
| `PrivateKey` | 44 characters: 43 of standard base64, then `=`, and the last character's two spare bits zero (as the reference tools requires); not all zero | used |
| `ListenPort` | 0-65535 | UDP source port, 0 = random |
| `FwMark` | `off`, decimal or `0x` hex up to 0xffffffff | ignored; noted if non-zero |
| `Address` | `a.b.c.d[/0-32]` (no prefix = /32) or IPv6 `[/0-128]`; empty entries skipped | first IPv4 used; later IPv4 and all IPv6 noted |
| `DNS` | digits and dots = IPv4 server (must be valid, not 0.0.0.0); contains `:` = IPv6 server (must be valid); anything else = search domain | first two IPv4 used; the rest noted |
| `MTU` | 576-65535; empty = default | used; above 1440 lowered to 1440 and noted |
| `Table` | anything; empty = default | `off`: add no routes; `auto`: normal; other values noted |
| `PreUp`, `PostUp`, `PreDown`, `PostDown` | anything, any length | noted: scripts cannot run on a Vita |
| `SaveConfig` | `true` or `false` | noted if true |
| `PublicKey` | as `PrivateKey` | used |
| `PresharedKey` | as `PrivateKey`, all zero allowed (= none) | used |
| `AllowedIPs` | entries `[+-]ip[/cidr]`, IPv4 cidr 0-32 (default 32), IPv6 0-128; empty entries are errors | IPv4 used, up to 32 distinct prefixes; host bits masked off and noted; duplicates noted; `-prefix` removes an earlier entry; IPv6 noted |
| `Endpoint` | `host:port`, port 1-65535 after the last `:` | dotted quad or host name kept for the app to resolve; `[v6]:port` and bare IPv6 rejected |
| `PersistentKeepalive` | `off` or 0-65535 | used, 0 = off |

The protocol keys need a value (`PrivateKey =` is an error, as in the reference tools);
the setup keys may be empty, as in the reference setup script, except `SaveConfig`.

### Rejected profiles

Besides the syntax errors above: no `PrivateKey`; no `[Peer]`; a second
`[Peer]` (one peer in v1); a peer without `PublicKey` or `Endpoint`; no
IPv4 `Address`; no IPv4 `AllowedIPs`; more than 32 IPv4 prefixes; a key in
the wrong section (`PublicKey` under `[Interface]`); a NUL byte anywhere;
a line longer than 2048 bytes once whitespace and comments are removed
(hook scripts excepted); and the handshake-obfuscation keys that some forks
add (`Jc`, `Jmin`, `Jmax`, `S1`-`S4`, `H1`-`H4`, `I1`-`I5`, `J1`-`J3`,
`Itime`), which a standard server would not understand.

### The vita-turbo-vpn directive

Comments are otherwise ignored, but a comment of the form

```
# vita-turbo-vpn: check = generic | https://...
```

anywhere in the file (also after a value) sets the connection-check preset.
Whitespace inside it is ignored, case does not matter except in the
URL, the last one wins, and a URL must start `https://` and be at most 159
printable characters. A malformed or unknown setting is a note, never an
error, so a hand-written comment cannot break a profile. Settings in
`profiles.meta` override it; that is the app's business.

## Where this differs from the reference tools, and why

| Case | the reference tools | here | Why |
|---|---|---|---|
| UTF-8 byte order mark at the start | error | skipped | some desktop editors add it; it cannot mean anything else |
| Bad key in an error | quotes the value | names the key only | the value may be the private key |
| `ListenPort`, `Endpoint` port | service names (`http`) accepted | numbers only | no services database; nobody writes them |
| `Endpoint` host like `1.2.3` | resolved by `getaddrinfo` (as 1.2.0.3) | rejected | resolvers disagree; only strict dotted quads |
| `Endpoint` port 0 | accepted | rejected | cannot be connected to |
| Host name characters | whatever the resolver takes | letters, digits, `-`, `_`, labels 1-63, total 253 | the app's resolver and file naming need a plain name |
| `Table = Off` (case) | reference setup script: only lower-case `off` | any case | keys and `off` elsewhere are case-insensitive |
| Spaces inside a setup key (`Save Config`) | reference setup script: not recognised | recognised | one whitespace rule for every key |
| `MTU` | anything `ip` accepts | 576-65535, lowered to 1440 | Wi-Fi is 1500; 1440 is the most a tunnel over IPv4 can carry |
| All-zero private or public key | accepted | rejected | cannot work |
| `FwMark` hex with leading zeros past 8 digits | accepted by `strtoul` | accepted | same |
| 32-bit `strtoul` saturation (`FwMark = 4294967296` on 32-bit hosts) | accepted as 0xffffffff | rejected | exact range check |

Plus the v1 restrictions (one peer, IPv4, a hostname resolved by the app),
which are deliberate rejections rather than differences in reading the file.

## Duplicate check and names

`vtc_same_text` treats two texts as the same profile when they are equal
after CRLF becomes LF and one trailing newline (LF or CRLF) is dropped.
A lone CR is kept, a second trailing newline counts, and comments and
formatting count: deduplication is literal on purpose.

`vtc_qr_file_name` builds the name from `endpoint_host`: characters outside
`A-Z a-z 0-9 . - _` become `_` (the parser lets none through anyway),
leading dots and trailing dots or dashes are dropped (`vpn.example.com.`
gives `vpn.example.com.conf`), and the host is cut to 64 characters. The
callback receives the bare file name; checking every storage and zip is the
app's job.

## Subnet clash

`vtc_subnet_clash` compares the profile's AllowedIPs with the Wi-Fi subnet:

- `VTC_CLASH_INSIDE`: a tunnel prefix lies inside the local subnet or equals
  it (home LAN `192.168.1.0/24` behind the tunnel, hotel Wi-Fi also
  `192.168.1.0/24`). Always worth a warning.
- `VTC_CLASH_SHADOWED`: a split tunnel's prefix contains the local subnet
  (`192.168.0.0/16` through the tunnel, Wi-Fi `192.168.1.0/24`), so that
  part of the remote range is out of reach on this network.
- A full tunnel containing the local subnet is not a clash: reaching the
  local network directly is the "allow local network" option.
  `Table = off` profiles add no routes and never clash.

## Tests

`make test`: 39 test functions covering provider-generated, home-server
and web-UI server layouts, NAS and router-style profiles, the official clients'
"exclude private addresses" list, CRLF files with a byte order mark,
whitespace and case variants, repeated keys and sections, comments and the
directive, every rejection, every key's boundaries, embedded NULs, lines of
hundreds of kilobytes, ten thousand entries, and 220,000 fuzzed inputs
checked against the result invariants. `make coverage` reports 100% of
lines and functions; the two branches left are defensive checks the parser
cannot reach.
