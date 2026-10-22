#!/usr/bin/env python3
"""Fetch pinned Ace 6T kernel inputs without Git history or archive copies."""
import argparse
import concurrent.futures
import hashlib
import json
import shutil
import tarfile
import time
import urllib.request
from pathlib import Path

GKI = "844001fb8721c3ee305f17a51628744997f787a0"
CLANG = "70a12c4fe88940ab2b0e6d763492dffa2f07e15a"
MODULES = "6265826aa0a52ea9a3bb60a84c3164dd61e282c7"
SOC = "2774f857d6e77443de95defbf1199e0afdc14d9a"
RUST = "5c78b1e636b2bf68d46a6980437590fddca64147"
CLANG_TOOLS = "c03d5bc912878cdcbdc38fcb003e94116b978956"
RESERVE = 4 * 1024**3
ROOT = Path(__file__).resolve().parents[2]


def display_headers(workspace):
    cached_tree = ROOT / "artifacts/ace6t/kernel-build-research/oneplus-modules-tree.json"
    if cached_tree.is_file():
        metadata = json.loads(cached_tree.read_text())
    else:
        url = f"https://api.github.com/repos/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8845/git/trees/{MODULES}?recursive=1"
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read(16 * 1024 * 1024 + 1)
        if len(data) > 16 * 1024 * 1024:
            raise ValueError("Upstream tree exceeds expected bound")
        metadata = json.loads(data)
    if metadata.get("truncated") or metadata.get("sha") != MODULES:
        raise ValueError("Need the complete tree at the pinned modules commit")
    tree = metadata["tree"]
    prefixes = ["vendor/oplus/kernel/boot/include/", "vendor/qcom/opensource/synx-kernel/",
                "vendor/qcom/opensource/securemsm-kernel/include/"]
    aliases = {
        "vendor/oplus/kernel/audio/qcom/oplus_typec_switch_i2c/common_reg.h": ["common_reg.h"],
        "vendor/oplus/kernel/audio/qcom/oplus_typec_switch_i2c/oplus_typec_switch_i2c.h": ["oplus_typec_switch_i2c.h"],
        "vendor/oplus/kernel/device_info/device_info/device_info.h": ["soc/oplus/device_info.h"],
        "vendor/oplus/kernel/touchpanel/touchpanel_notify/touchpanel_event_notify.h": ["soc/oplus/touchpanel_event_notify.h"],
    }
    selected = [item for item in tree if item["type"] == "blob" and item.get("mode") != "120000"
                and item["path"].endswith(".h") and (item["path"] in aliases or
                any(item["path"].startswith(prefix) for prefix in prefixes))]
    def download(item):
        path = item["path"]
        url = f"https://raw.githubusercontent.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8845/{MODULES}/{path}"
        target = workspace / "vendor" / path
        if target.exists():
            data = target.read_bytes()
        else:
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read()
        blob = hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()
        if blob != item["sha"]:
            raise ValueError(f"Pinned Git blob mismatch: {path}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        destinations = list(aliases.get(path, []))
        if path.startswith("vendor/oplus/kernel/boot/include/"):
            destinations += [f"soc/oplus/{kind}/{target.name}" for kind in ["boot", "system"]]
        for alias in destinations:
            destination = workspace / "display-headers" / alias
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        return {"url": url, "git_blob": blob, "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": len(data), "aliases": destinations}
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        receipts = list(pool.map(download, selected))
    (workspace / "display-headers.source.json").write_text(json.dumps(receipts, indent=2) + "\n")
    print(f"Prepared {len(receipts)} verified display dependency headers", flush=True)


class CheckedStream:
    def __init__(self, response, workspace):
        self.response = response
        self.workspace = workspace
        self.digest = hashlib.sha256()
        self.size = 0
        self.checked = 0.0

    def read(self, size=-1):
        now = time.monotonic()
        if now - self.checked > 5:
            free = shutil.disk_usage(self.workspace).free
            if free < RESERVE:
                raise RuntimeError("Stopped: fewer than 4 GiB free")
            print(f"received {self.size / 1024**2:.1f} MiB; free {free / 1024**3:.1f} GiB", flush=True)
            self.checked = now
        chunk = self.response.read(size)
        self.digest.update(chunk)
        self.size += len(chunk)
        return chunk


def fetch(workspace, name, url, strip=0, prefixes=None):
    target = workspace / name
    receipt = workspace / f"{name}.source.json"
    if receipt.exists():
        meta = json.loads(receipt.read_text())
        if meta["url"] != url or not target.is_dir():
            raise RuntimeError(f"Unexpected existing receipt: {receipt}")
        print(f"Already prepared: {target}", flush=True)
        return
    if target.exists():
        raise RuntimeError(f"Incomplete or unrelated destination exists: {target}")
    target.mkdir()
    count = 0
    unpacked = 0
    skipped_links = []
    with urllib.request.urlopen(url, timeout=120) as response:
        stream = CheckedStream(response, workspace)
        with tarfile.open(fileobj=stream, mode="r|gz") as archive:
            for member in archive:
                parts = Path(member.name).parts[strip:]
                if not parts:
                    continue
                relative = Path(*parts).as_posix()
                if prefixes and not any(relative == p.rstrip("/") or relative.startswith(p) for p in prefixes):
                    continue
                member.name = relative
                # These SoC convenience links point to vendor trees omitted
                # from this header-only checkout. Never follow them outside it.
                if name == "soc" and member.issym():
                    skipped_links.append({"path": relative, "target": member.linkname})
                    continue
                # Python's data filter rejects paths/symlinks outside target.
                archive.extract(member, target, filter="data")
                count += 1
                unpacked += member.size
        while stream.read(1024 * 1024):
            pass
    receipt.write_text(json.dumps({"url": url, "archive_sha256": stream.digest.hexdigest(),
                                   "download_bytes": stream.size, "file_bytes": unpacked,
                                   "members": count, "skipped_links": skipped_links}, indent=2) + "\n")
    print(f"Prepared {name}: {unpacked / 1024**2:.1f} MiB in {count} members", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--only", choices=["common", "clang", "vendor", "soc", "rust", "bindgen", "display-headers"])
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    if args.only == "display-headers":
        display_headers(workspace)
        return
    sources = {
        "common": (f"https://android.googlesource.com/kernel/common/+archive/{GKI}.tar.gz", 0, None),
        "clang": (f"https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/{CLANG}/clang-r536225.tar.gz", 0, None),
        "vendor": (f"https://codeload.github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8845/tar.gz/{MODULES}", 1,
                   ["vendor/qcom/opensource/display-drivers/", "kernel_platform/oplus/", "vendor/qcom/opensource/mm-drivers/"]),
        "soc": (f"https://codeload.github.com/OnePlusOSS/android_kernel_oneplus_sm8845/tar.gz/{SOC}", 1,
                ["include/", "drivers/clk/qcom/", "BUILD.bazel", "common_sources.bzl", "configs/"]),
        "rust": (f"https://android.googlesource.com/platform/prebuilts/rust/+archive/{RUST}/linux-x86/1.82.0.tar.gz", 0, None),
        "bindgen": (f"https://android.googlesource.com/platform/prebuilts/clang-tools/+archive/{CLANG_TOOLS}/linux-x86/bin.tar.gz", 0, ["bindgen"]),
    }
    for name, (url, strip, prefixes) in sources.items():
        if args.only is None or args.only == name:
            fetch(workspace, name, url, strip, prefixes)
    if args.only is None:
        display_headers(workspace)


if __name__ == "__main__":
    main()
