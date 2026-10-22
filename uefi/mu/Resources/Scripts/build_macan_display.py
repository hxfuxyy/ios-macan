#!/usr/bin/env python3
"""Reproduce the upstream Canoe display source selection using plain Kbuild."""
import argparse
import ast
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

from build_macan_kernel import ROOT, run
from check_macan_kernel_abi import Elf


def call_keywords(path, function):
    tree = ast.parse(path.read_text())
    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
             and isinstance(node.func, ast.Name) and node.func.id == function]
    if len(calls) != 1:
        raise ValueError(f"Ambiguous upstream declaration: {path}")
    values = {}
    for kw in calls[0].keywords:
        try:
            values[kw.arg] = ast.literal_eval(kw.value)
        except (ValueError, TypeError):
            # Names such as registry and target are irrelevant to source
            # selection; do not evaluate upstream Starlark as Python code.
            continue
    return values


def apply_pinned_patch(display, patch, marker):
    digest = hashlib.sha256(patch.read_bytes()).hexdigest()
    if marker.exists() and marker.read_text().strip() != digest:
        raise ValueError(f"Patch changed; inspect source: {patch.name}")
    # --batch alone silently ignores -R on an unapplied patch. --force keeps
    # direction explicit, so an unchanged module cannot be labelled patched.
    base = ["patch", "--force", "--fuzz=0", "-p1", "-i", str(patch)]
    reverse = subprocess.run(base + ["--dry-run", "--reverse"], cwd=display,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if reverse.returncode:
        run(base + ["--dry-run", "--forward"], display, os.environ.copy())
        run(base + ["--forward"], display, os.environ.copy())
        run(base + ["--dry-run", "--reverse"], display, os.environ.copy())
    marker.write_text(digest + "\n")
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--software-input-fence-fix", action="store_true",
                        help="Clear inherited hardware waits when input fences use software")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error("jobs must be between 1 and 16")
    workspace = args.workspace.resolve()
    source = workspace / "common"
    output = workspace / "out"
    display = workspace / "vendor/vendor/qcom/opensource/display-drivers"
    if not (workspace / "kernel-build.json").is_file():
        raise ValueError("Build the core kernel first")
    trace_patch = ROOT / "Resources/LinuxRamTest/ace6t-display-trace.patch"
    marker = workspace / "display-trace-patch.sha256"
    trace_hash = apply_pinned_patch(display, trace_patch, marker)
    software_hash = None
    software_marker = workspace / "display-software-input-fence-patch.sha256"
    if args.software_input_fence_fix:
        software_patch = ROOT / "Resources/LinuxRamTest/ace6t-display-software-input-fence.patch"
        software_hash = apply_pinned_patch(display, software_patch, software_marker)
    elif software_marker.exists():
        raise ValueError("Source contains the software input-fence fix; pass its explicit build option")
    target = call_keywords(display / "targets/canoe.bzl", "define_target_variant_modules")
    # registry/modules are variables in the upstream Starlark declaration;
    # call_keywords reads only literal keyword values below.
    options = set(target["config_options"]) | {"CONFIG_DRM_MSM"}
    module = call_keywords(display / "display_modules.bzl", "module_entry")
    if module["name"] != "msm_drm":
        raise ValueError("Unexpected display module")
    sources = list(module.get("srcs", []))
    for key, choices in module["config_srcs"].items():
        if isinstance(choices, list):
            if key in options:
                sources.extend(choices)
        elif key in options:
            for option, files in choices.items():
                if option is True or option in options:
                    sources.extend(files)
    sources = list(dict.fromkeys(path.strip() for path in sources))
    if not sources or any(not path.endswith(".c") or not (display / path).is_file() for path in sources):
        raise ValueError("Display source list is incomplete")
    kernel_symbols = {line.split()[1] for line in (output / "Module.symvers").read_text().splitlines()}
    records = {}
    for path in sorted((ROOT / "artifacts/ace6t/vendor-ramdisk-extracted/lib/modules").glob("*.ko")):
        if path.name == "msm_drm.ko":
            continue
        for name, item in Elf(path).export_records().items():
            if name not in kernel_symbols:
                if name in records and records[name]["crc"] != item["crc"]:
                    raise ValueError(f"Conflicting actual vendor export: {name}")
                records[name] = {**item, "provider": path.stem}
    symvers = workspace / "stock-vendor.symvers"
    symvers.write_text("".join(
        f"{v['crc']:#010x}\t{k}\t{v['provider']}\t{v['export']}\t{v['namespace']}\n"
        for k, v in sorted(records.items())))
    includes = [".", "include", "include/uapi/display", "rotator", "msm", "msm/dp", "msm/hdmi",
                "msm/dsi", "msm/dsi/hfi", "msm/hfi", "msm/sde", "msm/sde/hfi",
                "msm/iris/core/include", "msm/iris/core", "msm/iris/core/common", "msm/iris/core/iris7p",
                "msm/iris/vendor", "msm/iris/vendor/include", "msm/iris/vendor/common", "msm/iris/vendor/iris7p",
                "oplus/SM8850", "oplus/SM8850/include", "oplus/common/trackpoint",
                "../mm-drivers/hw_fence/include", "../mm-drivers/hfi_core/inc",
                "../mm-drivers/msm_ext_display/include", "../mm-drivers/sync_fence/include"]
    flags = [f"-I$(src)/{path}" for path in includes]
    flags += [f"-I{workspace}/soc/include", f"-I{workspace}/soc/drivers/clk/qcom"]
    flags += [f"-I{workspace}/display-headers",
              f"-I{workspace}/vendor/vendor/qcom/opensource/synx-kernel/msm/synx",
              f"-I{workspace}/vendor/vendor/qcom/opensource/synx-kernel/include/uapi/synx/media",
              f"-I{workspace}/vendor/vendor/qcom/opensource/securemsm-kernel/include",
              f"-I{workspace}/vendor/vendor/oplus/kernel/boot/include"]
    flags += [f"-D{name}=1" for name in sorted(options)]
    # The upstream DDK puts additional_msm_headers before common headers and
    # builds external modules with Canoe vendor Kconfig symbols. They are not
    # part of the GKI .config. Keep this overlay local to the display build.
    config_tree = ast.parse((workspace / "soc/configs/canoe_perf.bzl").read_text())
    assignments = [node for node in config_tree.body if isinstance(node, ast.Assign)
                   and any(isinstance(target, ast.Name) and target.id == "canoe_perf_config"
                           for target in node.targets)]
    if len(assignments) != 1:
        raise ValueError("Ambiguous Canoe vendor config")
    vendor_config = ast.literal_eval(assignments[0].value)
    core_config = {}
    for line in (output / ".config").read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            core_config[key] = value
    extra_config = {key: value for key, value in vendor_config.items()
                    if key not in core_config and value in ("y", "m") and key not in options}
    config_header = workspace / "display-vendor-config.h"
    config_text = "/* External Canoe vendor settings; GKI config stays unchanged. */\n" + \
                  "".join(f"#define {key}{'_MODULE' if value == 'm' else ''} 1\n"
                          for key, value in sorted(extra_config.items()))
    if not config_header.exists() or config_header.read_text() != config_text:
        config_header.write_text(config_text)
    flags += [f"-include {config_header}"]
    kbuild = display / "Kbuild"
    backup = display / "Kbuild.upstream"
    if not backup.exists():
        shutil.copyfile(kbuild, backup)
    kbuild.write_text("# Generated from pinned upstream Canoe DDK declarations.\n"
                      f"LINUXINCLUDE := -I{workspace}/soc/include -I{workspace}/soc/include/uapi $(LINUXINCLUDE)\n"
                      "obj-m := msm_drm.o\nmsm_drm-y := " + " ".join(path[:-2] + ".o" for path in sources) + "\n"
                      "ccflags-y += " + " ".join(flags) + "\n")
    env = os.environ.copy()
    env["PATH"] = ":".join([str(workspace / "clang/bin"), str(workspace / "rust/bin"),
                              str(workspace / "bindgen"), env["PATH"]])
    env["LIBCLANG_PATH"] = str(workspace / "clang/lib")
    env["KBUILD_GENDWARFKSYMS_STABLE"] = "1"
    env["KBUILD_BUILD_TIMESTAMP"] = "Fri Dec 5 02:15:49 UTC 2025"
    make = ["make", "-C", source, f"O={output}", "ARCH=arm64", "LLVM=1",
            "HOSTCFLAGS=-O2 -std=gnu11", "KCFLAGS=-D__ANDROID_COMMON_KERNEL__"]
    run(make + ["modules_prepare"], workspace, env)
    command = make + [f"M={display}", f"KBUILD_EXTRA_SYMBOLS={symvers}", f"-j{args.jobs}", "modules"]
    (workspace / "display-selection.json").write_text(json.dumps({
        "options": sorted(options), "sources": sources,
        "trace_patch_sha256": trace_hash,
        "software_input_fence_patch_sha256": software_hash,
        "vendor_config_overlay": extra_config,
        "built": False,
    }, indent=2) + "\n")
    run(command, workspace, env)
    module_path = display / "msm_drm.ko"
    selection_path = workspace / "display-selection.json"
    selection = json.loads(selection_path.read_text())
    selection.update(built=True, module_bytes=module_path.stat().st_size,
                     module_sha256=hashlib.sha256(module_path.read_bytes()).hexdigest(),
                     phone_loaded=False)
    selection_path.write_text(json.dumps(selection, indent=2) + "\n")
    print(f"Display module built: {module_path}; CRC/dependency/boot checks pending", flush=True)


if __name__ == "__main__":
    main()
