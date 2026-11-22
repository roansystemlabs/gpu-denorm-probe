"""gpu-denorm-probe: does your D3D12 GPU keep or flush f32/f16 subnormals?

Builds shaders/denorm_probe_f32.hlsl and shaders/denorm_probe_f16.hlsl with DXC at ps_6_2,
once with no -denorm flag and once each with -denorm any|ftz|preserve, records the denormal
attributes DXC wrote into the DXIL, draws each build with the small D3D12 runner on the chosen
adapter(s) and classifies every row as `preserved`, `flushed (+0)` / `flushed (-0)` or `other`.

Usage:
    python -I probe.py [--backend hardware|warp|both] [--adapter NAME]
                       [--dxc PATH] [--runner PATH] [--out DIR] [--json FILE]

Exit codes: 0 ok; 1 a build failed to compile or draw; 2 negative control failed (the f32
`-denorm ftz` build did not flush the required add row on some adapter); 3 tools not found.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Roan System Labs
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SHADERS = HERE / "shaders"
MODES = [None, "any", "ftz", "preserve"]

# Rows: (name, channel, shift, width_mask, preserve_value, required)
PROBES = {
    "f32": {
        "source": "denorm_probe_f32.hlsl", "flags": [],
        "constants": (0x00000001, 0x007FFFFF, 0x80000000, 0x00800000, 0x3F000000, 0x00800001, 0x80800000, 0),
        "rows": [("add 0x00800001 + 0x80800000", 0, 0, 0xFFFFFFFF, 0x00000001, True),
                 ("min(least, largest)", 1, 0, 0xFFFFFFFF, 0x00000001, False),
                 ("max(least, -0)", 2, 0, 0xFFFFFFFF, 0x00000001, False),
                 ("least normal * 0.5", 3, 0, 0xFFFFFFFF, 0x00400000, False)],
    },
    "f16": {
        "source": "denorm_probe_f16.hlsl", "flags": ["-enable-16bit-types"],
        "constants": (0x0001, 0x03FF, 0x8000, 0x0400, 0x3800, 0x33800000, 0x0401, 0x8400),
        "rows": [("add 0x0401 + 0x8400", 0, 0, 0xFFFF, 0x0001, True),
                 ("min(0x0001, 0x03ff)", 0, 16, 0xFFFF, 0x0001, False),
                 ("max(0x0001, -0)", 1, 0, 0xFFFF, 0x0001, False),
                 ("0x0400 * 0.5", 1, 16, 0xFFFF, 0x0200, False),
                 ("f32(f16 0x0001)", 2, 0, 0xFFFFFFFF, 0x33800000, False),
                 ("f16(f32 2^-24)", 3, 0, 0xFFFF, 0x0001, False),
                 ("move(0x0001) [mode-independent]", 3, 16, 0xFFFF, 0x0001, False)],
    },
}


def classify(value: int, want: int, mask: int) -> str:
    sign = 0x80000000 if mask > 0xFFFF else 0x8000
    if value == want:
        return "preserved"
    if value & (mask & ~sign) == 0:
        return f"flushed ({'-' if value & sign else '+'}0)"
    return "other"


def find_dxc(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    on_path = shutil.which("dxc")
    if on_path:
        return on_path
    kits = Path(os.environ.get("ProgramFiles(x86)", "")) / "Windows Kits" / "10" / "bin"
    candidates = sorted(kits.glob("10.*/x64/dxc.exe"), key=lambda p: [int(x) for x in p.parent.parent.name.split(".")])
    return str(candidates[-1]) if candidates else None


def find_runner(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    for candidate in (HERE / "build" / "probe_runner.exe", HERE / "build" / "Release" / "probe_runner.exe"):
        if candidate.exists():
            return str(candidate)
    return shutil.which("probe_runner")


def compile_shader(dxc: str, source: Path, target: str, flags: list[str], out: Path, listing: Path | None):
    cmd = [dxc, *flags, "-T", target, "-E", "main", "-Fo", str(out)]
    if listing is not None:
        cmd += ["-Fc", str(listing)]
    return subprocess.run(cmd + [str(source)], capture_output=True, text=True)


def run_runner(runner: str, backend: str, adapter: str | None, vs: Path, ps: Path, constants) -> dict:
    cmd = [runner, "--backend", backend, "--vs", str(vs), "--ps", str(ps),
           "--constants", ",".join(f"{c:x}" for c in constants)]
    if adapter and backend == "hardware":
        cmd += ["--adapter", adapter]
    r = subprocess.run(cmd, capture_output=True, text=True)
    info = {"rc": r.returncode}
    for line in r.stdout.splitlines():
        key, _, value = line.partition("=")
        info[key.strip()] = value.strip()
    if r.returncode != 0 and "error" not in info:
        info["error"] = (r.stdout + r.stderr).strip()[:300]
    return info


def probe_backend(dxc, runner, backend, adapter, work: Path, vs: Path) -> dict:
    result = {"backend": backend, "adapter": None, "builds": []}
    for name, probe in PROBES.items():
        for mode in MODES:
            flags = probe["flags"] + (["-denorm", mode] if mode else [])
            stem = f"{name}_{mode or 'default'}"
            ps, listing = work / f"{stem}.cso", work / f"{stem}.lst"
            build = {"type": name, "denorm_flag": mode or "(none)"}
            c = compile_shader(dxc, SHADERS / probe["source"], "ps_6_2", flags, ps, listing)
            if c.returncode:
                build["error"] = f"dxc rc={c.returncode}: {c.stderr.strip()[:300]}"
                result["builds"].append(build)
                continue
            text = listing.read_text(errors="replace")
            build["dxil_attrs"] = sorted(set(re.findall(r'"fp\d+-denorm-mode"="\w+"', text)))
            info = run_runner(runner, backend, adapter, vs, ps, probe["constants"])
            if result["adapter"] is None and "adapter" in info:
                result["adapter"] = {k: info.get(k) for k in
                                     ("adapter", "vendor", "device", "driver", "shader_model", "native_16bit")}
            if info["rc"] != 0 or "words" not in info:
                build["error"] = f"runner rc={info['rc']}: {info.get('error', '?')}"
                result["builds"].append(build)
                continue
            words = [int(w, 16) for w in info["words"].split()]
            build["words"] = [f"{w:08x}" for w in words]
            build["rows"] = []
            for row, ch, shift, mask, want, required in probe["rows"]:
                value = (words[ch] >> shift) & mask
                build["rows"].append({"row": row, "required": required,
                                      "value": f"{value:0{8 if mask > 0xFFFF else 4}x}",
                                      "expected_if_preserved": f"{want:0{8 if mask > 0xFFFF else 4}x}",
                                      "state": classify(value, want, mask)})
            result["builds"].append(build)
    ftz = next((b for b in result["builds"] if b["type"] == "f32" and b["denorm_flag"] == "ftz"), None)
    result["negative_control"] = bool(ftz and "rows" in ftz and ftz["rows"][0]["state"].startswith("flushed"))
    return result


def print_backend(result: dict) -> None:
    a = result["adapter"] or {}
    print(f"== {result['backend']}: {a.get('adapter', '?')} vendor={a.get('vendor', '?')} "
          f"device={a.get('device', '?')} driver={a.get('driver', '?')} "
          f"sm={a.get('shader_model', '?')} native16={a.get('native_16bit', '?')}")
    for build in result["builds"]:
        label = f"[{build['type']}] ps_6_2 " + (f"-denorm {build['denorm_flag']}"
                                                  if build["denorm_flag"] != "(none)" else "(no -denorm)")
        if "error" in build:
            print(f"{label}: {build['error']}")
            continue
        print(f"{label}: dxil_attrs={build['dxil_attrs'] or ['(none)']}")
        for row in build["rows"]:
            print(f"    {'REQUIRED ' if row['required'] else '         '}{row['row']:34s} "
                  f"{row['value']:>8s}  {row['state']}")
    print("NEGATIVE CONTROL " + ("passed: f32 -denorm ftz flushed the required add row"
                                 if result["negative_control"] else
                                 "FAILED: f32 -denorm ftz did not flush the required add row"))


def dxc_version(dxc: str) -> str:
    r = subprocess.run([dxc, "--version"], capture_output=True, text=True)
    return (r.stdout or r.stderr).strip().splitlines()[0] if (r.stdout or r.stderr).strip() else "?"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--backend", choices=["hardware", "warp", "both"], default="both")
    parser.add_argument("--adapter", help="substring of the hardware adapter name (default: first hardware adapter)")
    parser.add_argument("--dxc", help="path to dxc.exe (default: PATH, then the newest Windows SDK)")
    parser.add_argument("--runner", help="path to probe_runner.exe (default: build/probe_runner.exe)")
    parser.add_argument("--out", help="keep compiled shaders and DXIL listings in this directory")
    parser.add_argument("--json", help="write the full result as JSON to this file")
    args = parser.parse_args()

    dxc, runner = find_dxc(args.dxc), find_runner(args.runner)
    if not dxc or not runner:
        print(f"tools not found: dxc={dxc} runner={runner} (build the runner first, see README)")
        return 3

    work = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="gpu-denorm-probe-"))
    work.mkdir(parents=True, exist_ok=True)
    vs = work / "fullscreen_vs.cso"
    c = compile_shader(dxc, SHADERS / "fullscreen_vs.hlsl", "vs_6_0", [], vs, None)
    if c.returncode:
        print(f"vertex shader failed to compile: {c.stderr.strip()[:300]}")
        return 1

    report = {"tool": "gpu-denorm-probe", "os": f"{platform.system()} {platform.version()}",
              "dxc": dxc_version(dxc), "results": []}
    print(f"os={report['os']}  dxc={report['dxc']}")
    for backend in (["hardware", "warp"] if args.backend == "both" else [args.backend]):
        result = probe_backend(dxc, runner, backend, args.adapter, work, vs)
        report["results"].append(result)
        print_backend(result)

    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not args.out:
        shutil.rmtree(work, ignore_errors=True)
    if any("error" in b for r in report["results"] for b in r["builds"]):
        return 1
    return 0 if all(r["negative_control"] for r in report["results"]) else 2


if __name__ == "__main__":
    sys.exit(main())
