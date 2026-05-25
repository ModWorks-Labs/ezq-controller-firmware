from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RELEASE_DIR = ROOT / "release"
VERSION_FILE = RELEASE_DIR / "version.txt"
MANIFEST_FILE = RELEASE_DIR / "ezq-update-manifest.json"
MANIFEST_TEMPLATE = ROOT / "manifests" / "ezq-update-manifest.template.json"
BUILD_BIN = ROOT / ".pio" / "build" / "ota-lan" / "firmware.bin"

BOARD_ID = "EZQ-CTLR-B"
CHANNEL = "stable"
REPO_RAW_BASE = (
    "https://raw.githubusercontent.com/ModWorks-Labs/ezq-controller-firmware/main/release"
)


def resolve_pio() -> str:
    pio = shutil.which("pio") or shutil.which("pio.exe")
    if pio:
        return pio

    candidate = Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe"
    if candidate.exists():
        return str(candidate)

    raise FileNotFoundError("PlatformIO CLI not found. Expected pio on PATH or in ~/.platformio/penv/Scripts.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bump firmware version, rebuild, and stage release artifacts."
    )
    parser.add_argument("version", help="Semver to embed and stage, for example 1.1.0-alpha.4")
    parser.add_argument(
        "--environment",
        default="ota-lan",
        help="PlatformIO environment to build. Default: ota-lan",
    )
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="Skip the clean build step.",
    )
    return parser.parse_args()


def run_command(command: list[str]) -> None:
    subprocess.run(command, cwd=ROOT, check=True)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def read_manifest_seed() -> dict:
    source = MANIFEST_FILE if MANIFEST_FILE.exists() else MANIFEST_TEMPLATE
    return json.loads(source.read_text(encoding="utf-8"))


def compute_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_embedded_version(path: Path, version: str) -> None:
    data = path.read_bytes()
    if version.encode("ascii") not in data:
        raise RuntimeError(
            f"Built firmware does not contain version string {version!r}. "
            "Run failed before staging release artifacts."
        )


def stage_firmware(version: str) -> tuple[Path, str, int]:
    if not BUILD_BIN.exists():
        raise FileNotFoundError(f"Build output not found: {BUILD_BIN}")

    ensure_embedded_version(BUILD_BIN, version)

    asset_name = f"ezq-controller-firmware_{BOARD_ID}_{version}.bin"
    asset_path = RELEASE_DIR / asset_name

    for old_asset in RELEASE_DIR.glob(f"ezq-controller-firmware_{BOARD_ID}_*.bin"):
        if old_asset != asset_path:
            old_asset.unlink()

    shutil.copy2(BUILD_BIN, asset_path)
    sha256 = compute_sha256(asset_path)
    size = asset_path.stat().st_size
    return asset_path, sha256, size


def update_manifest(version: str, sha256: str, size: int, asset_name: str) -> None:
    manifest = read_manifest_seed()
    manifest["schema_version"] = 1
    manifest["generated_at"] = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )
    manifest["channel"] = CHANNEL
    manifest.setdefault("boards", {})
    manifest["boards"][BOARD_ID] = {
        "version": version,
        "firmware_url": f"{REPO_RAW_BASE}/{asset_name}",
        "sha256": sha256,
        "size": size,
    }

    write_text(MANIFEST_FILE, json.dumps(manifest, indent=2) + "\n")


def main() -> int:
    args = parse_args()
    pio = resolve_pio()
    previous_version = VERSION_FILE.read_text(encoding="utf-8").strip() if VERSION_FILE.exists() else ""

    try:
        write_text(VERSION_FILE, args.version + "\n")

        if not args.no_clean:
            run_command([pio, "run", "-e", args.environment, "-t", "clean"])
        run_command([pio, "run", "-e", args.environment])

        asset_path, sha256, size = stage_firmware(args.version)
        update_manifest(args.version, sha256, size, asset_path.name)

        print(f"Prepared release {args.version}")
        print(f"Version file: {VERSION_FILE.relative_to(ROOT)}")
        print(f"Manifest: {MANIFEST_FILE.relative_to(ROOT)}")
        print(f"Firmware: {asset_path.relative_to(ROOT)}")
        print(f"SHA256: {sha256}")
        print(f"Size: {size}")
        return 0
    except Exception as exc:
        if previous_version:
            write_text(VERSION_FILE, previous_version + "\n")
        elif VERSION_FILE.exists():
            VERSION_FILE.unlink()
        print(f"Release prep failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
