"""Regenerate fixed FG/OpenXR/UI bytecode with FXC; normal builds need no FXC.

Run: python tools/compile-fg-shaders.py [--fxc PATH]
Use --check on any platform to check the checked-in source hashes.
"""
from pathlib import Path
import argparse
import ast
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "shared/generated/fg_shader_bytecode.hpp"


def raw(name):
    text = (ROOT / "shared" / name).read_text(encoding="utf-8-sig")
    literals = re.findall(r'R"([A-Za-z0-9_]*)\((.*?)\)\1"', text, re.S)
    if len(literals) != 1:
        raise ValueError(f"Expected one raw HLSL literal in {name}")
    return literals[0][1]


def symbol(file, name):
    text = (ROOT / file).read_text(encoding="utf-8-sig")
    pattern = r'\b' + re.escape(name) + r'\[\]\s*=\s*(CHEEKY_FG_COMPACT_HLSL\s*)?R"([A-Za-z0-9_]*)\((.*?)\)\2"'
    matches = list(re.finditer(pattern, text, re.S))
    if not matches:
        raise ValueError(f"Missing HLSL {name} in {file}")
    values = [(raw("fg_compact.hpp") if m[1] else "") + m[3] for m in matches]
    if len(set(values)) != 1:
        raise ValueError(f"Ambiguous HLSL {name} in {file}")
    return values[0]


def overlay_symbol(dx, name):
    text = (ROOT / f"third_party/reshade/deps/imgui/backends/imgui_impl_dx{dx}.cpp").read_text(encoding="utf-8-sig")
    pattern = r'\b' + name + r'\[\]\s*=\s*(CHEEKY_OVERLAY_COLOR_HLSL\s*)?("(?:\\.|[^"\\])*")'
    match = re.search(pattern, text, re.S)
    if not match:
        raise ValueError(f"Missing ImGui shader {name}, DX{dx}")
    source = ast.literal_eval(match[2].replace("\\\n", ""))
    if match[1]:
        color = (ROOT / "third_party/reshade/deps/imgui/backends/cheeky_overlay_color.h").read_text()
        prefix = re.search(r'R"HLSL\((.*?)\)HLSL"', color, re.S)
        source = prefix[1] + source
    return source


def fnv(text):
    value = 14695981039346656037
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fxc", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    compact = raw("fg_compact.hpp")
    sources = {
        "optical": compact + raw("fg_optical_shader.hpp"),
        "interpolation": compact + raw("fg_interpolation_shader.hpp"),
        "snapshot": raw("fg_snapshot_shader.hpp"),
    }
    for name, file, variable in [
        ("focus_prepare", "openxr_layer/fg_focus.cpp", "prepare_shader"),
        ("focus_compose", "openxr_layer/fg_focus.cpp", "compose_shader"),
        ("focus_pack", "openxr_layer/fg_focus.cpp", "pack_shader"),
        ("focus_recenter", "openxr_layer/fg_focus.cpp", "recenter_shader"),
        ("color_draw", "openxr_layer/fg_color_draw.hpp", "shader"),
        ("ngx_fallback", "openxr_layer/fg_ngx_fallback.hpp", "shader"),
        ("quad_capture", "openxr_layer/quad_capture.hpp", "shader_source"),
        ("quad_fg", "openxr_layer/quad_fg.hpp", "shaders"),
        ("menu_vertex", "openxr_layer/openxr_layer.cpp", "vertex_shader"),
        ("menu_pixel", "openxr_layer/openxr_layer.cpp", "pixel_shader"),
        ("prepared_guides", "src/fg_prepared_guides.hpp", "shader"),
        ("peripheral_full", "src/d3d11_peripheral_dlaa.cpp", "shader"),
    ]:
        sources[name] = symbol(file, variable)
    for dx in (11, 12):
        sources[f"overlay_vertex{dx}"] = overlay_symbol(dx, "vertexShader")
        sources[f"overlay_pixel{dx}"] = overlay_symbol(dx, "pixelShader")
    hashes = [f"inline constexpr std::uint64_t {name}_source_hash = 0x{fnv(source):016x}ull;"
              for name, source in sources.items()]
    if args.check:
        generated = OUTPUT.read_text(encoding="utf-8")
        for line in hashes:
            if line not in generated:
                raise SystemExit("FG bytecode is stale. Run python tools/compile-fg-shaders.py on Windows.")
        print("PASS: all precompiled FG shader source hashes match")
        return
    fxc = args.fxc
    if not fxc:
        sdk = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits/10/bin"
        candidates = sorted(sdk.glob("*/x64/fxc.exe"), key=lambda p: tuple(map(int, p.parts[-3].split("."))))
        if not candidates:
            raise SystemExit("FXC not found. Install a Windows SDK or pass --fxc PATH.")
        fxc = candidates[-1]
    variants = [("optical", entry, profile) for profile in ("cs_5_0", "cs_5_1")
                for entry in ("Prepare", "Interpolate", "CacheVectors")]
    variants += [("snapshot", "Main", "cs_5_0"),
                 ("interpolation", "RegionsVS", "vs_5_0"),
                 ("interpolation", "RegionsPS", "ps_5_0")]
    variants += [("interpolation", entry, "cs_5_1") for entry in ("Main", "CenterMain")]
    variants += [(name, "Main", "cs_5_1") for name in ("focus_prepare", "focus_compose", "focus_pack", "focus_recenter", "ngx_fallback")]
    variants += [("color_draw", entry, profile) for entry, profile in (("VS", "vs_5_1"), ("PS", "ps_5_1"))]
    variants += [("quad_capture", entry, profile) for entry, profile in
                 (("VS", "vs_5_0"), ("PS", "ps_5_0"), ("VS", "vs_5_1"), ("PS", "ps_5_1"))]
    variants += [("quad_fg", entry, "cs_5_0") for entry in ("Snapshot", "Atlas")]
    variants += [(name, "main", profile) for name, profile in
                 (("menu_vertex", "vs_5_0"), ("menu_pixel", "ps_5_0"), ("menu_vertex", "vs_5_1"), ("menu_pixel", "ps_5_1"))]
    variants += [("prepared_guides", "Main", "cs_5_1_depth0"), ("prepared_guides", "Main", "cs_5_1_depth1"),
                 ("peripheral_full", "Main", "cs_5_0")]
    for dx, model in ((11, "4_0"), (12, "5_0")):
        variants.append((f"overlay_vertex{dx}", "main", f"vs_{model}"))
        variants += [(f"overlay_pixel{dx}", "main", f"ps_{model}_color{mode}") for mode in range(4)]
    result = ["// Generated by tools/compile-fg-shaders.py with FXC (O3; UI converters O1). Do not edit.",
              "#pragma once", "#include <cstdint>", "namespace cheeky::fg::bytecode {", *hashes]
    total = 0
    work_root = (ROOT / "build/fg-shaders").resolve()
    work_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="compile-", dir=work_root) as temp:
        work = Path(temp)
        if not work.resolve().is_relative_to(work_root):
            raise RuntimeError("Shader work directory escaped the build directory")
        for name, source in sources.items():
            (work / f"{name}.hlsl").write_text(source, encoding="utf-8", newline="\n")
        for source, entry, profile in variants:
            name = f"{source}_{entry.lower()}_{profile}"
            blob = work / f"{name}.cso"
            options = ["/D", "DEPTH_ARRAY=" + profile[-1]] if "_depth" in profile else ["/D", "CHEEKY_COLOR_MODE=" + profile[-1]] if "_color" in profile else []
            target = profile.split("_depth")[0].split("_color")[0]
            optimization = "/O1" if source.startswith(("menu_", "overlay_")) else "/O3"
            process = subprocess.run([str(fxc), "/nologo", optimization, *options, "/T", target, "/E", entry,
                                      "/Fo", str(blob), str(work / f"{source}.hlsl")],
                                     capture_output=True, text=True)
            if process.returncode:
                raise SystemExit(process.stdout + process.stderr)
            data = blob.read_bytes()
            if not data.startswith(b"DXBC"):
                raise SystemExit(f"Unexpected bytecode format for {name}")
            total += len(data)
            result.append(f"inline constexpr unsigned char {name}[] = {{")
            for offset in range(0, len(data), 20):
                result.append("    " + ",".join(f"0x{value:02x}" for value in data[offset:offset + 20]) + ",")
            result.append("};")
            print(f"{name}: {len(data)} bytes", flush=True)
    result.append("}")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text("\n".join(result) + "\n", encoding="utf-8", newline="\n")
    print(f"Wrote {OUTPUT} ({total} bytes of DXBC)")


if __name__ == "__main__":
    main()
