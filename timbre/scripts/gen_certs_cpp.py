#!/usr/bin/env python3
"""Genera lib/Certs/Certs.cpp a partir de lib/Certs/certs/{ca,device,device-key}.pem.

Uso (desde timbre/):  python3 scripts/gen_certs_cpp.py

El CN de device.pem tiene que ser igual a DEVICE_ID en platformio.ini:
Mosquitto usa el CN como username y el ACL solo deja tocar home/<CN>/#.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "lib" / "Certs" / "certs"
OUT = ROOT / "lib" / "Certs" / "Certs.cpp"

FILES = [("CA_CERT", "ca.pem"), ("DEVICE_CERT", "device.pem"), ("DEVICE_KEY", "device-key.pem")]

parts = ['#include "Certs.h"', "#include <pgmspace.h>", "",
         "// Generado por scripts/gen_certs_cpp.py a partir de lib/Certs/certs/*.pem.",
         "// No editar a mano. Esta en .gitignore: no commitear.", ""]
for name, fname in FILES:
    path = SRC / fname
    if not path.exists():
        sys.exit(f"Falta {path}")
    pem = path.read_text().strip()
    if ")EOF" in pem:
        sys.exit(f"{fname} contiene ')EOF'")
    parts.append(f'const char {name}[] PROGMEM = R"EOF({pem}\n)EOF";\n')

OUT.write_text("\n".join(parts))
print(f"OK -> {OUT.relative_to(ROOT)}")
