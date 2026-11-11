#!/usr/bin/env python3
"""make_fixture.py - a made-up profiles tree for `make shots-files`, laid
out as the console's storages (ux0/, uma0/), so the Profiles screens can be
rendered from real files through the same code as on the console.

Every key is random bytes from a seeded generator, never a real key; hosts
are example.org / example.net or documentation addresses (RFC 5737).

    python3 tools/make_fixture.py <out dir>
"""
import base64
import os
import random
import sys
import zipfile

rng = random.Random(20261029)


def key():
    return base64.b64encode(bytes(rng.randrange(1, 256) for _ in range(32))).decode()


def conf(address, endpoint, allowed="0.0.0.0/0", dns="192.0.2.1", extra_iface="", extra_peer=""):
    lines = ["[Interface]", f"PrivateKey = {key()}", f"Address = {address}"]
    if dns:
        lines.append(f"DNS = {dns}")
    if extra_iface:
        lines.append(extra_iface)
    lines += ["", "[Peer]", f"PublicKey = {key()}", f"Endpoint = {endpoint}",
              f"AllowedIPs = {allowed}", "PersistentKeepalive = 25"]
    if extra_peer:
        lines.append(extra_peer)
    return "\n".join(lines) + "\n"


def write(path, text, crlf=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\r\n" if crlf else "\n") as f:
        f.write(text)


def main():
    out = sys.argv[1]
    ux0 = os.path.join(out, "ux0")
    uma0 = os.path.join(out, "uma0")
    home = conf("10.8.0.2/24", "home.example.org:51820", dns="10.8.0.1")
    write(os.path.join(ux0, "Home server.conf"), home)
    write(os.path.join(ux0, "Cloud VPS.conf"),
          conf("10.9.0.2/32", "198.51.100.20:51820", dns="10.9.0.1",
               extra_iface="PostUp = iptables -A FORWARD -i tun0 -j ACCEPT\nAddress = fd00::2/128"))
    write(os.path.join(ux0, "Home LAN only.conf"),
          conf("10.8.0.3/32", "home.example.org:51820", allowed="192.168.1.0/24", dns=""), crlf=True)
    write(os.path.join(ux0, "friend.conf"),
          conf("10.7.0.9/32", "vpn.example.net:51820", extra_iface="Jc = 4\nJmin = 40"))
    write(os.path.join(ux0, "notes.txt"), "not a profile\n")
    write(os.path.join(ux0, "._Home server.conf"), "desktop metadata\n")
    write(os.path.join(ux0, "home", "Media server.conf"),
          conf("10.8.0.4/32", "home.example.org:51821", allowed="10.20.0.0/16", dns=""))
    write(os.path.join(ux0, "home", "NAS.conf"),
          conf("10.8.0.5/32", "home.example.org:51822", allowed="10.20.0.0/16, 10.30.0.0/16", dns=""))
    with zipfile.ZipFile(os.path.join(ux0, "travel.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        shared = key()
        for i, host in enumerate(["192.0.2.44", "192.0.2.88", "192.0.2.120"], 1):
            text = conf("192.0.2.34/32", f"{host}:51820")
            text = text.replace(text.split("\n")[1], f"PrivateKey = {shared}")
            z.writestr(f"exit-{i}.conf", text)
        z.writestr("README.txt", "made-up bundle\n")
        z.writestr("__MACOSX/._exit-1.conf", "metadata\n")
    write(os.path.join(uma0, "Cloud VPS.conf"),
          conf("10.9.0.2/32", "198.51.100.20:51820", dns="10.9.0.1"))


if __name__ == "__main__":
    main()
