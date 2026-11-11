#!/usr/bin/env python3
"""Regenerate vectors.h from the published sources.

Usage: python3 gen_vectors.py <dir> vectors.h, where <dir> holds:

  rfc8439.txt         https://www.rfc-editor.org/rfc/rfc8439.txt
  rfc7748.txt         https://www.rfc-editor.org/rfc/rfc7748.txt
  rfc7693.txt         https://www.rfc-editor.org/rfc/rfc7693.txt
  xchacha.txt         https://www.ietf.org/archive/id/draft-irtf-cfrg-xchacha-03.txt
  blake2s-kat.txt     https://raw.githubusercontent.com/BLAKE2/BLAKE2/master/testvectors/blake2s-kat.txt
  siphash_vectors.h   https://raw.githubusercontent.com/veorq/SipHash/master/vectors.h

The HMAC-BLAKE2s and parameterised BLAKE2s vectors come from Python's own
hashlib and hmac, and the small-order X25519 points are checked against a
plain RFC 7748 ladder, both below.
"""
import re, sys, hashlib, hmac
V = sys.argv[1]
def L(name): return open(f"{V}/{name}").read().split("\n")
PAGE = re.compile(r"^(Nir & Langley|RFC 8439|Langley, et al\.|RFC 7748|Saarinen|RFC 7693|Arciszewski|Internet-Draft)\b")
DUMP = re.compile(r"^\s*(\d{3})\s{2}((?:[0-9a-f]{2} ){0,15}[0-9a-f]{2})(\s|$)")
def find(lines, label, start=0):
    for i in range(start, len(lines)):
        if label in lines[i]:
            return i
    raise KeyError(label)
def dump(lines, label, start=0):
    """offset-style hex dump following a label line"""
    i = find(lines, label, start) + 1
    out = bytearray(); got = False
    while i < len(lines):
        s = lines[i]
        m = DUMP.match(s)
        if m:
            assert int(m.group(1)) == len(out), (label, s)
            out += bytes.fromhex(m.group(2)); got = True
        elif got and (s.strip() == "" or PAGE.match(s.strip())):
            # allow page breaks inside a dump
            j = i
            while j < len(lines) and (lines[j].strip() == "" or PAGE.match(lines[j].strip())):
                j += 1
            if j < len(lines) and DUMP.match(lines[j]) and int(DUMP.match(lines[j]).group(1)) == len(out):
                i = j; continue
            break
        elif got:
            break
        i += 1
    return bytes(out)
def plainhex(lines, label, start=0):
    i = find(lines, label, start) + 1
    out = ""; got = False
    while i < len(lines):
        s = lines[i].strip()
        if re.fullmatch(r"[0-9a-fA-F ]+", s) and s:
            out += s.replace(" ", ""); got = True
        elif got and s == "":
            # a blank inside spaced-hex (A.3 tv 5..11) ends the field
            break
        elif got:
            break
        i += 1
    return bytes.fromhex(out)

C = []
def arr(name, b):
    C.append(f"static const uint8_t {name}[{len(b)}] = {{")
    for k in range(0, len(b), 12):
        C.append("\t" + " ".join(f"0x{x:02x}," for x in b[k:k+12]))
    C.append("};")

r = L("rfc8439.txt")
# 2.4.2 ChaCha20 encryption
s = find(r, "2.4.2.  Example and Test Vector for the ChaCha20 Cipher", 600)
arr("rfc8439_242_key", bytes(range(32)))
arr("rfc8439_242_nonce", bytes.fromhex("000000000000004a00000000"))
arr("rfc8439_242_pt", dump(r, "Plaintext Sunscreen:", s))
arr("rfc8439_242_ct", dump(r, "Ciphertext Sunscreen:", s))
# 2.5.2 Poly1305
s = find(r, "2.5.2.  Poly1305 Example and Test Vector", 800)
arr("rfc8439_252_key", bytes.fromhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b"))
arr("rfc8439_252_msg", dump(r, "Message to be Authenticated:", s))
arr("rfc8439_252_tag", bytes.fromhex(r[find(r, "Tag: a8:06", s)].split("Tag:")[1].strip().replace(":", "")))
# 2.6.2 Poly1305 key generation
s = find(r, "2.6.2.  Poly1305 Key Generation Test Vector", 1000)
arr("rfc8439_262_key", dump(r, "Key:", s))
arr("rfc8439_262_nonce", dump(r, "Nonce:", s))
arr("rfc8439_262_otk", dump(r, "Output bytes:", s))
# 2.8.2 AEAD
s = find(r, "2.8.2.  Example and Test Vector for AEAD_CHACHA20_POLY1305", 1200)
arr("rfc8439_282_pt", dump(r, "Plaintext:", s))
arr("rfc8439_282_aad", dump(r, "AAD:", s))
arr("rfc8439_282_key", dump(r, "Key:", s))
arr("rfc8439_282_iv", dump(r, "IV:", s))
arr("rfc8439_282_fixed", dump(r, "32-bit fixed-common part:", s))
arr("rfc8439_282_ct", dump(r, "Ciphertext:", s))
t = find(r, "Tag:", find(r, "AEAD Construction for Poly1305:", s))
arr("rfc8439_282_tag", bytes.fromhex("".join(re.findall(r"[0-9a-f]{2}", r[t+1]))))
# A.3 Poly1305 vectors 1-11
s = find(r, "A.3.  Poly1305 Message Authentication Code", 1900)
polys = []
for n in range(1, 12):
    v = find(r, f"Test Vector #{n}:", s)
    if n <= 4:
        key = dump(r, "One-time Poly1305 Key:", v)
        msg = dump(r, "Text to MAC:", v)
        tag = dump(r, "Tag:", v)
    else:
        key = plainhex(r, "R:", v) + plainhex(r, "S:", v)
        msg = plainhex(r, "data:", v)
        tag = plainhex(r, "tag:", v)
    assert len(key) == 32 and len(tag) == 16, (n, len(key), len(tag))
    arr(f"rfc8439_a3_{n}_key", key); arr(f"rfc8439_a3_{n}_msg", msg); arr(f"rfc8439_a3_{n}_tag", tag)
    polys.append(n)
    s = v
C.append("#define RFC8439_A3_VECTOR(n) { rfc8439_a3_##n##_key, rfc8439_a3_##n##_msg, sizeof(rfc8439_a3_##n##_msg), rfc8439_a3_##n##_tag }")
# A.5 AEAD decryption
s = find(r, "A.5.  ChaCha20-Poly1305 AEAD Decryption", 2300)
arr("rfc8439_a5_key", dump(r, "The ChaCha20 Key", s))
arr("rfc8439_a5_ct", dump(r, "Ciphertext:", s))
arr("rfc8439_a5_nonce", dump(r, "The nonce:", s))
arr("rfc8439_a5_aad", dump(r, "The AAD:", s))
arr("rfc8439_a5_tag", dump(r, "Received Tag:", s))
arr("rfc8439_a5_pt", dump(r, "Plaintext::", s))

x = L("xchacha.txt")
s = find(x, "A.3.1.  AEAD_XCHACHA20_POLY1305", 700)
for f, lab in [("pt", "Plaintext:"), ("aad", "AAD:"), ("key", "Key:"), ("iv", "IV:"), ("ct", "Ciphertext:"), ("tag", "Tag:")]:
    arr(f"xchacha_a31_{f}", plainhex(x, lab, s))
arr("xchacha_221_key", bytes(range(32)))
arr("xchacha_221_nonce", bytes.fromhex("000000090000004a0000000031415927"))
words = "82413b42 27b27bfe d30e4250 8a877d73 a0f9e4d5 8a74a853 c12ec413 26d3ecdc".split()
assert any("82413b42 27b27bfe d30e4250 8a877d73" in l for l in x)
arr("xchacha_221_out", bytes.fromhex("".join(words)))

x = L("rfc7748.txt")
s = find(x, "5.2.  Test Vectors", 500)
def after(lines, label, start):
    i = find(lines, label, start)
    return bytes.fromhex(lines[i+1].strip()), i
k1, i = after(x, "Input scalar:", s); u1, i = after(x, "Input u-coordinate:", i); o1, i = after(x, "Output u-coordinate:", i)
k2, i = after(x, "Input scalar:", i); u2, i = after(x, "Input u-coordinate:", i); o2, i = after(x, "Output u-coordinate:", i)
for n, (a, b, c) in enumerate([(k1, u1, o1), (k2, u2, o2)], 1):
    arr(f"rfc7748_52_{n}_scalar", a); arr(f"rfc7748_52_{n}_u", b); arr(f"rfc7748_52_{n}_out", c)
it1, i = after(x, "After one iteration:", s); it1000, i = after(x, "After 1,000 iterations:", s)
arr("rfc7748_iter1", it1); arr("rfc7748_iter1000", it1000)
s = find(x, "6.1.  Curve25519", 700)
for f, lab in [("a", "Alice's private key, a:"), ("ka", "Alice's public key, X25519(a, 9):"), ("b", "Bob's private key, b:"), ("kb", "Bob's public key, X25519(b, 9):"), ("k", "Their shared secret, K:")]:
    v, _ = after(x, lab, s); arr(f"rfc7748_61_{f}", v)

b = L("rfc7693.txt")
i = find(b, 'BLAKE2s-256("abc") =', 800)
abc = bytes.fromhex(b[i].split("=")[1] + b[i+1])
arr("rfc7693_abc", abc)
i = find(b, "const uint8_t blake2s_res[32] = {", 1500)
res = bytes(int(t, 16) for t in re.findall(r"0x([0-9A-F]{2})", " ".join(b[i+1:i+5])))
assert len(res) == 32
arr("rfc7693_selftest_res", res)

# Reference BLAKE2s keyed KAT (BLAKE2 repository, testvectors/blake2s-kat.txt)
kat = open(f"{V}/blake2s-kat.txt").read().strip().split("\n\n")
hashes = []
for n, blk in enumerate(kat):
    d = dict(l.split(":", 1) for l in blk.strip().split("\n"))
    assert bytes.fromhex(d["in"].strip()) == bytes(range(n)) and bytes.fromhex(d["key"].strip()) == bytes(range(32))
    hashes.append(bytes.fromhex(d["hash"].strip()))
assert len(hashes) == 256
C.append("/* hash i = BLAKE2s-256(key = 00..1f, in = 00..(i-1)) */")
C.append("static const uint8_t blake2s_kat[256][32] = {")
for h in hashes:
    C.append("\t{ " + ", ".join(f"0x{x:02x}" for x in h) + " },")
C.append("};")

# SipHash-2-4 reference vectors (veorq/SipHash vectors.h)
t = open(f"{V}/siphash_vectors.h").read()
t = t[t.index("vectors_sip64"):t.index("vectors_sip128")]
vals = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", t)]
assert len(vals) == 512
C.append("/* vector i = SipHash-2-4(key = 00..0f, in = 00..(i-1)), little-endian */")
C.append("static const uint8_t siphash24_vec[64][8] = {")
for k in range(64):
    C.append("\t{ " + ", ".join(f"0x{x:02x}" for x in vals[8*k:8*k+8]) + " },")
C.append("};")

# HMAC-BLAKE2s and short keyed BLAKE2s, from Python's hashlib/hmac.
def seq(n, m=0): return bytes((7 * k + m) & 0xff for k in range(n))
hm = []
for kl, ml in [(32, 0), (32, 1), (32, 32), (32, 64), (32, 65), (1, 3), (64, 100), (65, 33), (100, 200)]:
    key = seq(kl, 1); msg = seq(ml, 2)
    hm.append((kl, ml, hmac.new(key, msg, hashlib.blake2s).digest()))
C.append("/* HMAC-BLAKE2s-256(key = seq(klen, 1), msg = seq(mlen, 2)), seq(n, m)[k] = 7k + m */")
C.append("static const struct { size_t klen, mlen; uint8_t mac[32]; } hmac_blake2s_vec[] = {")
for kl, ml, d in hm:
    C.append(f"\t{{ {kl}, {ml}, {{ " + ", ".join(f"0x{x:02x}" for x in d) + " } },")
C.append("};")
kb = []
for kl, ol, ml in [(32, 16, 0), (32, 16, 116), (16, 16, 132), (32, 32, 65), (0, 16, 64), (0, 32, 1000)]:
    key = seq(kl, 3); msg = seq(ml, 4)
    kb.append((kl, ol, ml, hashlib.blake2s(msg, key=key, digest_size=ol).digest()))
C.append("/* BLAKE2s(outlen, key = seq(klen, 3), msg = seq(mlen, 4)) */")
C.append("static const struct { size_t klen, outlen, mlen; uint8_t out[32]; } blake2s_param_vec[] = {")
for kl, ol, ml, d in kb:
    C.append(f"\t{{ {kl}, {ol}, {ml}, {{ " + ", ".join(f"0x{x:02x}" for x in d) + " } },")
C.append("};")

# Small-order points: X25519 of any scalar with these is all zero. Checked
# here with a straight RFC 7748 ladder before they are emitted.
P = 2**255 - 19
def x25519(k, u):
    k = bytearray(k); k[0] &= 248; k[31] &= 127; k[31] |= 64
    k = int.from_bytes(k, "little"); u = int.from_bytes(u, "little") & ((1 << 255) - 1)
    x1, x2, z2, x3, z3, sw = u, 1, 0, u, 1, 0
    for t in reversed(range(255)):
        kt = (k >> t) & 1; sw ^= kt
        if sw: x2, x3, z2, z3 = x3, x2, z3, z2
        sw = kt
        A = x2 + z2; AA = A*A; B = x2 - z2; BB = B*B; E = AA - BB
        C_ = x3 + z3; D = x3 - z3; DA = D*A; CB = C_*B
        x3 = (DA + CB)**2 % P; z3 = x1 * (DA - CB)**2 % P
        x2 = AA * BB % P; z2 = E * (AA + 121665 * E) % P
    if sw: x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, P - 2, P) % P).to_bytes(32, "little")
assert x25519(k1, u1) == o1 and x25519(k2, u2) == o2
low = [bytes(32), (1).to_bytes(32, "little"),
       bytes.fromhex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"),
       bytes.fromhex("5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157")]
for pt in low:
    assert x25519(bytes(range(32)), pt) == bytes(32) and x25519(k1, pt) == bytes(32), pt.hex()
C.append("/* u-coordinates of small-order points (X25519 result is all zero) */")
C.append("static const uint8_t x25519_low_order[4][32] = {")
for pt in low:
    C.append("\t{ " + ", ".join(f"0x{x:02x}" for x in pt) + " },")
C.append("};")

hdr = """/*
 * vectors.h - published test vectors, generated from the source documents:
 *
 *   RFC 8439 (ChaCha20, Poly1305, AEAD): sections 2.4.2, 2.5.2, 2.6.2,
 *     2.8.2, appendix A.3 (Poly1305 #1-#11) and A.5
 *   draft-irtf-cfrg-xchacha-03: 2.2.1 (HChaCha20), A.3.1 (XChaCha20-Poly1305)
 *   RFC 7748 (X25519): 5.2 (two vectors, 1 and 1,000 iterations), 6.1;
 *     small-order points checked against an RFC 7748 ladder in Python
 *   RFC 7693 (BLAKE2s): appendix B ("abc") and the appendix E grand hash
 *   BLAKE2 reference repository: testvectors/blake2s-kat.txt (keyed KAT)
 *   SipHash reference repository: vectors.h (SipHash-2-4, 64-bit output)
 *   HMAC-BLAKE2s and parameterised keyed BLAKE2s: computed with Python's
 *     hashlib.blake2s and hmac, there being no published set
 *
 * Do not edit by hand.
 */
"""
open(sys.argv[2], "w").write(hdr + "#ifndef VECTORS_H\n#define VECTORS_H\n\n#include <stddef.h>\n#include <stdint.h>\n\n" + "\n".join(C) + "\n\n#endif /* VECTORS_H */\n")
