#!/usr/bin/env python3
"""Compare actual stock module import CRCs with rebuilt kernel exports."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:6] != b"\x7fELF\x02\x01" or len(self.data) < 64:
            raise ValueError(f"Expected little-endian ELF64: {path}")
        if struct.unpack_from("<H", self.data, 18)[0] != 183:
            raise ValueError(f"Expected AArch64: {path}")
        offset = struct.unpack_from("<Q", self.data, 40)[0]
        size, count, names = struct.unpack_from("<HHH", self.data, 58)
        if size != 64 or not 0 < count < 4096 or names >= count:
            raise ValueError("Unsupported section table")
        self.bounds(offset, count * size)
        self.headers = [struct.unpack_from("<IIQQQQIIQQ", self.data, offset + i * size)
                        for i in range(count)]
        self.names = self.section_data(names)
        self.sections = {self.string(self.names, h[0]): i for i, h in enumerate(self.headers)}

    def bounds(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError("ELF range outside file")

    @staticmethod
    def string(data, offset):
        if not 0 <= offset < len(data):
            raise ValueError("String offset outside table")
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError("Unterminated string")
        return data[offset:end].decode("ascii")

    def section_data(self, index):
        h = self.headers[index]
        if h[1] == 8:
            raise ValueError("Cannot read NOBITS section")
        self.bounds(h[4], h[5])
        return self.data[h[4]:h[4] + h[5]]

    def named(self, name):
        return self.section_data(self.sections[name]) if name in self.sections else b""

    def imports(self):
        result = {}
        legacy = self.named("__versions")
        if len(legacy) % 64:
            raise ValueError("Malformed legacy symbol versions")
        for offset in range(0, len(legacy), 64):
            crc = struct.unpack_from("<Q", legacy, offset)[0]
            name = self.string(legacy[offset + 8:offset + 64], 0)
            result[name] = crc
        extended = self.named("__version_ext_crcs")
        names = self.named("__version_ext_names")
        if extended:
            if len(extended) % 4 or not names.endswith(b"\0"):
                raise ValueError("Malformed extended symbol versions")
            symbols = names.rstrip(b"\0").split(b"\0")
            if len(symbols) != len(extended) // 4:
                raise ValueError("Extended CRC/name count mismatch")
            for i, raw in enumerate(symbols):
                name = raw.decode("ascii")
                crc = struct.unpack_from("<I", extended, i * 4)[0]
                if name in result and result[name] != crc:
                    raise ValueError(f"Conflicting CRCs for {name}")
                result[name] = crc
        if not result:
            raise ValueError(f"No import CRCs: {self.path}")
        return result

    def exports(self):
        return {name: item["crc"] for name, item in self.export_records().items()}

    def export_records(self):
        result = {}
        if ".symtab" not in self.sections:
            return result
        table_index = self.sections[".symtab"]
        table = self.section_data(table_index)
        strings = self.section_data(self.headers[table_index][6])
        if len(table) % 24:
            raise ValueError("Malformed symbol table")
        symbols = {}
        for offset in range(0, len(table), 24):
            name_offset, info, other, section, value, size = struct.unpack_from("<IBBHQQ", table, offset)
            name = self.string(strings, name_offset)
            symbols[name] = (section, value)
        for name, (section, value) in symbols.items():
            if not name.startswith("__crc_"):
                continue
            if section >= len(self.headers):
                raise ValueError(f"Unsupported CRC section for {name}")
            data = self.section_data(section)
            if value + 4 > len(data):
                raise ValueError("CRC offset outside section")
            exported = name[6:]
            namespace = ""
            namespace_symbol = symbols.get("__kstrtabns_" + exported)
            if namespace_symbol:
                ns_section, ns_value = namespace_symbol
                namespace = self.string(self.section_data(ns_section), ns_value)
            export_symbol = symbols.get("__ksymtab_" + exported)
            if not export_symbol:
                raise ValueError(f"Export table symbol missing: {exported}")
            export_section = export_symbol[0]
            gpl = self.string(self.names, self.headers[export_section][0]) == "__ksymtab_gpl"
            result[exported] = {"crc": struct.unpack_from("<I", data, value)[0],
                                "export": "EXPORT_SYMBOL_GPL" if gpl else "EXPORT_SYMBOL",
                                "namespace": namespace}
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-symvers", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--display-module", type=Path,
                        help="Check a rebuilt msm_drm.ko replacing the captured stock copy")
    args = parser.parse_args()
    kernel = {}
    for line in args.kernel_symvers.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 4:
            kernel[fields[1]] = int(fields[0], 16)
    if not kernel:
        raise ValueError("Empty kernel exports")
    stock = {}
    for directory in [ROOT / "artifacts/ace6t/vendor-ramdisk-extracted/lib/modules",
                      ROOT / "artifacts/ace6t/android-after-ram-test"]:
        for path in sorted(directory.glob("*.ko")):
            elf = Elf(path)
            imports = elf.imports()
            if path.name in stock and stock[path.name][0] != imports:
                raise ValueError(f"Conflicting stock module copies: {path.name}")
            stock[path.name] = (imports, path)
    display_comparison = None
    if args.display_module:
        original = Elf(stock["msm_drm.ko"][1])
        rebuilt = Elf(args.display_module)
        a, b = original.imports(), rebuilt.imports()
        x, y = original.export_records(), rebuilt.export_records()
        display_comparison = {
            "stock_sha256": hashlib.sha256(original.data).hexdigest(),
            "rebuilt_sha256": hashlib.sha256(rebuilt.data).hexdigest(),
            "imports_identical": a == b, "exports_identical": x == y,
            "stock_imports": len(a), "rebuilt_imports": len(b),
            "stock_exports": len(x), "rebuilt_exports": len(y),
        }
        if a != b or x != y:
            raise ValueError("Rebuilt display interface differs; review before packaging")
        stock["msm_drm.ko"] = (b, args.display_module)
    providers = {}
    for imports, path in stock.values():
        for symbol, crc in Elf(path).exports().items():
            if symbol in providers and providers[symbol][0] != crc:
                raise ValueError(f"Conflicting stock export CRCs: {symbol}")
            providers[symbol] = (crc, path.name)
    selected = set()
    for name in ["usb", "display", "touch"]:
        selected.update((ROOT / f"Resources/LinuxRamTest/{name}-modules.load").read_text().splitlines())
    selected.add("qcom_dynamic_ramoops.ko")
    errors = []
    modules = {}
    for name in sorted(selected):
        if name not in stock:
            errors.append({"module": name, "error": "missing stock module"})
            continue
        imports, path = stock[name]
        matched = 0
        dependencies = set()
        for symbol, crc in sorted(imports.items()):
            if symbol in kernel:
                if kernel[symbol] != crc:
                    errors.append({"module": name, "symbol": symbol, "error": "kernel CRC mismatch",
                                   "stock_crc": hex(crc), "kernel_crc": hex(kernel[symbol])})
                else:
                    matched += 1
            elif symbol in providers:
                expected, provider = providers[symbol]
                if crc != expected or provider not in selected:
                    errors.append({"module": name, "symbol": symbol, "error": "stock provider mismatch or not selected",
                                   "provider": provider, "expected_crc": hex(expected), "import_crc": hex(crc)})
                dependencies.add(provider)
            else:
                errors.append({"module": name, "symbol": symbol, "error": "unresolved import"})
        modules[name] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                         "imports": len(imports), "matched_kernel_imports": matched,
                         "stock_dependencies": sorted(dependencies)}
    report = {"kernel_symvers_sha256": hashlib.sha256(args.kernel_symvers.read_bytes()).hexdigest(),
              "kernel_exports": len(kernel), "modules": modules, "errors": errors,
              "display_comparison": display_comparison,
              "crc_check_passed": not errors,
              "limitation": "CRC agreement alone does not validate module loading, CFI, signatures or hardware behavior"}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Modules: {len(modules)}; kernel exports: {len(kernel)}; errors: {len(errors)}")
    if errors:
        for error in errors[:12]:
            print(error)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
