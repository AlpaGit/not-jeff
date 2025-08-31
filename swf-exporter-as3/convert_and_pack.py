#!/usr/bin/env python3
import argparse, subprocess, shutil, json
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

# --- Réglages DDS / texconv ---
TEXCONV_EXE = shutil.which("texconv") or "texconv"
DEFAULT_FORMAT = "DXT5"   # alternatives utiles: "DXT5", "BC3_UNORM"
PREMULTIPLY_ALPHA = False
GEN_MIPMAPS = False            # True si tu veux des mipmaps

def run_texconv(png_path: Path, dds_path: Path, fmt: str, premul: bool, mipmaps: bool):
    # texconv écrit les sorties dans un dossier via -o, et garde le nom de base
    out_dir = dds_path.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [TEXCONV_EXE,
           "-f", fmt,
           "-o", str(out_dir),
           "-nologo"]
    if premul:
        cmd += ["-pmalpha"]
    if not mipmaps:
        cmd += ["-m", "1"]     # 1 mip level => pas de mipmaps

    cmd += [str(png_path)]
    print("TEXCONV:", " ".join(cmd))
    subprocess.check_call(cmd)

    # texconv produit <basename>.DDS (majuscule souvent). On renomme si besoin.
    produced = out_dir / (png_path.stem + ".DDS")
    if produced.exists():
        produced.rename(dds_path)

def convert_json_pages_to_dds(json_path: Path, root_dir: Path, fmt: str, premul: bool, mipmaps: bool):
    data = json.loads(json_path.read_text(encoding="utf-8"))
    updated = False

    def convert_path_list(path_list):
        nonlocal updated
        new_pages = []
        for p in path_list:
            # résout le fichier relatif au root
            p_abs = (root_dir / p) if (root_dir / p).exists() else None
            if p_abs and p_abs.suffix.lower() == ".png":
                dds_abs = p_abs.with_suffix(".dds")
                if not dds_abs.exists():
                    run_texconv(p_abs, dds_abs, fmt, premul, mipmaps)
                new_pages.append(str(Path(p).with_suffix(".dds")).replace("\\", "/"))
                updated = True
            else:
                new_pages.append(p)
        return new_pages

    # Global pages
    if isinstance(data.get("pages"), list):
        data["pages"] = convert_path_list(data["pages"])

    # Per-symbol pages
    if isinstance(data.get("symbols"), list):
        for sym in data["symbols"]:
            if isinstance(sym.get("pages"), list):
                sym["pages"] = convert_path_list(sym["pages"])

    if updated:
        json_path.write_text(json.dumps(data, indent=2), encoding="utf-8")

def build_pak(root_dir: Path, pak_path: Path):
    with ZipFile(pak_path, "w", compression=ZIP_DEFLATED, compresslevel=9) as z:
        for p in root_dir.rglob("*"):
            if p.is_file():
                # on peut exclure les PNG si leur DDS existe
                if p.suffix.lower() == ".png":
                    dds = p.with_suffix(".dds")
                    if dds.exists():
                        continue
                arc = str(p.relative_to(root_dir)).replace("\\", "/")
                z.write(p, arcname=arc)
    print("PAK built:", pak_path)

def main():
    ap = argparse.ArgumentParser(description="Convert PNG atlases -> DDS (BC7/DXT5) and pack to .pak (zip)")
    ap.add_argument("root", help="Root folder that contains JSON + PNG pages")
    ap.add_argument("--pak", default="assets.pak", help="Output pak path (zip)")
    ap.add_argument("--format", default=DEFAULT_FORMAT, help="DDS format for texconv (e.g. BC7_UNORM, DXT5, BC3_UNORM)")
    ap.add_argument("--no-premul", action="store_true", help="Disable premultiplied alpha (default: on)")
    ap.add_argument("--mipmaps", action="store_true", help="Generate mipmaps")
    args = ap.parse_args()

    if shutil.which(TEXCONV_EXE) is None and TEXCONV_EXE == "texconv":
        raise SystemExit("texconv not found in PATH. Install DirectXTex (texconv) and ensure 'texconv' is available.")

    root = Path(args.root).resolve()
    fmt = args.format
    premul = not args.no_premul
    mipmaps = args.mipmaps

    # 1) Convert all PNG pages referenced by every swf-level JSON (ex: 431.json, 494.json, etc.)
    for json_file in root.rglob("*.json"):
        convert_json_pages_to_dds(json_file, root, fmt, premul, mipmaps)

    # 2) Build pak
    pak = Path(args.pak).resolve()
    build_pak(root, pak)

if __name__ == "__main__":
    main()
