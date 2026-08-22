#!/usr/bin/env python3
"""Generate samples/*/src/device_credentials.h from a device certificate + key.

Usage:
    python tools/gen_device_credentials.py <device-cert.pem> <device-key.pem> \
        [-o <output.h>]

The output header bakes the device identity into the compiled-credentials
sample builds (telemetry, c2d-led, click-telemetry on boards without
CONFIG_IOTCONNECT_IDENTITY_NVS). It contains the device PRIVATE KEY --
never commit it; the samples' src/ directories gitignore it. The public
broker/DRA CA roots come from the SDK's include/iotconnect_ca_roots.h,
which the generated header includes.

Default output: samples/telemetry/src/device_credentials.h (relative to
this SDK checkout) -- the shared location every compiled-credentials
sample includes from.
"""

import argparse
import os

BS = chr(92)  # backslash, built via chr() so no escaping ambiguity
NL = BS + "n"


def c_string(path):
    with open(path, "r", encoding="utf-8") as f:
        data = f.read()
    data = data.replace(chr(13) + chr(10), chr(10)).replace(chr(13), chr(10))
    data = data.replace(BS, BS + BS)
    data = data.replace('"', BS + '"')
    data = data.replace(chr(10), NL)
    return '"' + data + '"'


def decl(name, path):
    return "static const char " + name + "[] =\n\t" + c_string(path) + ";\n"


def main():
    default_out = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "samples", "telemetry", "src", "device_credentials.h"))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cert", help="device certificate PEM file")
    ap.add_argument("key", help="device private key PEM file")
    ap.add_argument("-o", "--out", default=default_out,
                    help="output header (default: %(default)s)")
    args = ap.parse_args()

    parts = [
        "/* AUTO-GENERATED device credentials. DO NOT COMMIT (contains the",
        " * device PRIVATE KEY). Regenerate with"
        " tools/gen_device_credentials.py. */",
        "#ifndef DEVICE_CREDENTIALS_H",
        "#define DEVICE_CREDENTIALS_H",
        "",
        '#include "iotconnect_ca_roots.h" /* public broker/DRA roots (SDK) */',
        "",
        decl("device_cert_pem", args.cert),
        decl("device_key_pem", args.key),
        "#endif /* DEVICE_CREDENTIALS_H */",
        "",
    ]
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(parts))
    print("wrote", args.out, os.path.getsize(args.out), "bytes")


if __name__ == "__main__":
    main()
