#!/usr/bin/env python3
"""Render the 2.79" UI's screens to PNG, or check them against the references.

    tools/ui_preview/ui_preview.py            # writes docs/en/images/ui/*.png
    tools/ui_preview/ui_preview.py --check    # fails if any screen changed

Builds tools/ui_preview with the host compiler (cmake, a C/C++ compiler and
Pillow are needed); the screens come from main/ui, as in the firmware.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
REFERENCE = ROOT / "docs" / "en" / "images" / "ui"
SCALE = 2  # the panel's pixels are small; the PNGs are drawn at twice the size


def build(build_dir: pathlib.Path) -> pathlib.Path:
    subprocess.run(["cmake", "-S", str(HERE), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(build_dir), "-j"], check=True, stdout=subprocess.DEVNULL)
    return build_dir / "ui_preview"


def render(tool: pathlib.Path, out: pathlib.Path) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(tool), str(out)], check=True)
    images = {}
    for ppm in sorted(out.glob("*.ppm")):
        img = Image.open(ppm).convert("RGB")
        images[ppm.stem] = img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST)
    return images


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="compare with the reference PNGs instead of writing them")
    parser.add_argument("--build-dir", type=pathlib.Path, help="where to build (default: a temporary directory)")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        build_dir = args.build_dir or pathlib.Path(tmp) / "build"
        images = render(build(build_dir), pathlib.Path(tmp) / "ppm")

    if not args.check:
        REFERENCE.mkdir(parents=True, exist_ok=True)
        for old in REFERENCE.glob("*.png"):
            if old.stem not in images:
                old.unlink()
        for name, img in images.items():
            img.save(REFERENCE / f"{name}.png", optimize=True)
        print(f"Wrote {len(images)} screens to {REFERENCE.relative_to(ROOT)}")
        return 0

    changed = []
    for name, img in images.items():
        ref_path = REFERENCE / f"{name}.png"
        if not ref_path.exists():
            changed.append(f"{name}: new")
            continue
        ref = Image.open(ref_path).convert("RGB")
        if ref.size != img.size or ImageChops.difference(ref, img).getbbox() is not None:
            changed.append(f"{name}: differs")
    for ref_path in REFERENCE.glob("*.png"):
        if ref_path.stem not in images:
            changed.append(f"{ref_path.stem}: no longer drawn")

    if changed:
        print("Screens changed:\n  " + "\n  ".join(changed))
        return 1
    print(f"All {len(images)} screens match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
