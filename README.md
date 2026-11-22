# gpu-denorm-probe

A small, standalone D3D12 probe that tells you whether a GPU **keeps or flushes subnormal
(denormal) floats** in pixel shaders, for binary32 (`float`) and binary16 (`half`), under each
DXC `-denorm` mode.

It compiles two synthetic HLSL shaders with DXC at `ps_6_2` (no flag, `-denorm any`, `ftz`,
`preserve`), draws each one on the adapter you pick and on WARP, reads the result back as raw
integer words, and prints a table plus optional JSON that you can compare between machines.

## Why it matters

- DXIL has exactly one float-mode function attribute, `"fp32-denorm-mode"="any|ftz|preserve"`.
  Without `-denorm` DXC writes no attribute, and the driver is free to flush. There is **no
  binary16 attribute at all**, so whether `half` subnormals survive is a property of the driver
  and hardware, not something a shader can request.
- Emulators, translators and test harnesses that compare GPU output bit-for-bit against a CPU
  reference need to know which mode they actually got. A difference of one subnormal is a
  difference of one bit, and it is invisible in an 8-bit colour readback.
- Common controls do not work: `x * 1.0f` folds to a move, and the DXIL text for `min`/`max`
  allows the result to be flushed or not. The probe's **required row is an add of two normal
  numbers whose exact sum is subnormal** (DXIL's FTZ text requires that result to be flushed),
  with `min`, `max` and `least normal * 0.5` as supplementary rows. For f16 it also checks
  widening to f32, narrowing from f32 and a plain move (a mode-independent control).
- Every operand comes from a constant buffer, so nothing can be folded at compile time, and the
  results go to an `R32G32B32A32_UINT` target, so the readback is lossless.

## Requirements

- Windows 10/11 with a D3D12 GPU (shader model 6.2; `native_16bit=1` for the f16 shader).
- DXC: the one in the Windows SDK (`Windows Kits\10\bin\<version>\x64\dxc.exe`) or a
  [DirectXShaderCompiler release](https://github.com/microsoft/DirectXShaderCompiler/releases).
  Keep `dxil.dll` next to `dxc.exe` so the shaders are signed.
- MSVC (Visual Studio or Build Tools with the C++ workload) to build the runner.
- Python 3.9 or later, standard library only.

## Usage

```bat
build.cmd
python -I probe.py
```

`build.cmd` finds MSVC through `vswhere` and writes `build\probe_runner.exe`
(CMake works too: `cmake -B build-cmake && cmake --build build-cmake --config Release`).

On an interactive terminal the probe starts with a short header (in cyan where the console
supports ANSI colours, including Windows 10 and later), then the measurement follows. A WARP run
looks like this:

```text
 \\      //
  \\    //    Roan System Labs · gpu-denorm-probe v1.0.0
   \\  //     https://roansystem.com
    \\//
     \/

os=Windows 10.0.26200  dxc=dxcompiler.dll: 1.9(5184-21d28f72)(1.9.2602.17) - 1.9.2602.17 (21d28f727)
== warp: Microsoft Basic Render Driver vendor=1414 device=008c driver=10.0.26100.9818 sm=6.2 native16=1
[f32] ps_6_2 (no -denorm): dxil_attrs=['(none)']
    REQUIRED add 0x00800001 + 0x80800000        00000000  flushed (+0)
...
```

The header is skipped with `--no-banner`, with `--json`, and whenever the output is redirected
or piped, so scripts see only the results. `NO_COLOR` turns the colour off.

Options of `probe.py`:

| Option | Meaning |
| --- | --- |
| `--backend hardware\|warp\|both` | which adapters to run (default `both`) |
| `--adapter NAME` | substring of the hardware adapter name, e.g. `"RX 9070"` (default: the first hardware adapter) |
| `--dxc PATH` | `dxc.exe` to use (default: `PATH`, then the newest Windows SDK) |
| `--runner PATH` | `probe_runner.exe` to use (default: `build\probe_runner.exe`) |
| `--out DIR` | keep the compiled shaders and DXIL listings |
| `--json FILE` | write the full result (adapter, driver, DXC version, raw words, classes) as JSON |
| `--no-banner` | skip the start-up header |
| `--version` | print the version and exit |

Each row is classified as `preserved`, `flushed (+0)` / `flushed (-0)` (a zero of either sign) or
`other`. Exit codes: `0` ok, `1` a build failed to compile or draw (for example a driver that
refuses the pipeline state for one build; the step and HRESULT are printed), `2` negative control
failed (the f32 `-denorm ftz` build did not flush the required add row), `3` tools not found.

`probe_runner.exe` can also be used on its own:
`probe_runner --backend hardware|warp [--adapter NAME] --vs VS.cso --ps PS.cso --constants 1,7fffff,...`
prints the adapter, driver and the four 32-bit words of the single pixel it drew.

## Results so far

All rows are from the same two shaders. Our RTX 3070 row was measured with this standalone
runner and matched, bit for bit on all 88 rows, the run we had made earlier through the
Xbox2Windows renderer. The other rows were measured by their authors with the Xbox2Windows
renderer (`offscreen_check`) and the shader files we published in
[Xbox2Windows #312](https://github.com/R0Y00/Xbox2Windows/issues/312); see their comments for
the exact environments.

| Adapter (vendor:device) | Driver | f32: no flag / `any` / `ftz` | f32: `preserve` | f16: all four builds | Reported by |
| --- | --- | --- | --- | --- | --- |
| NVIDIA GeForce RTX 3070 (`10de:2484`) | 32.0.16.2101 | all four rows flushed to `+0` | all preserved | all preserved | Roan System Labs, [comment](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7078022429) and this repository |
| NVIDIA GeForce RTX 3080 (`10de:2206`) | 32.0.16.1742 | all flushed to `+0` | all preserved | all preserved | @MoonYueya, [comment](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7078370705) |
| AMD Radeon(TM) Graphics, RDNA2 iGPU (`1002:164e`) | 32.0.11024.2 | all flushed to `+0` | all preserved | all preserved | @MoonYueya, [comment](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7078370705) |
| AMD Radeon RX 9070 XT (`1002:7550`) | 32.0.32029.2010 | all flushed to `+0` | all preserved | all preserved | @rwZephyr, [comment](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7083868420) |
| WARP (`1414:008c`) | 10.0.26100.9818 | flushed; `max(least, -0)` gives `-0` | all preserved | all preserved | Roan System Labs (this repository); @rwZephyr, [comment](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7083868420) |
| WARP (`1414:008c`) | 10.0.26100.9711 | flushed; `max(least, -0)` gives `-0` | all preserved | all preserved | @MoonYueya, [comment](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7078370705) |

"Preserved" values: f32 add/min/max `0x00000001`, multiply `0x00400000`; f16 add/min/max/narrow/move
`0x0001`, multiply `0x0200`, widen `0x33800000`. In every build DXC recorded only
`fp32-denorm-mode` (nothing without a flag), and on every adapter above an f32 build without a
flag behaved like `ftz`.

Related observations from the same thread, not produced by this probe:
@MoonYueya's [earlier RDNA2 compute-shader probe](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7077778245)
also found f16 subnormals unchanged, and pointed out the DXIL `min`/`max` caveat that made the
add row the required one; @abilliontokens [reported](https://github.com/R0Y00/Xbox2Windows/issues/312#issuecomment-7070826910)
an RTX 5060 Ti driver (32.0.16.2124) refusing pipeline creation for a different shader built
with `-denorm preserve` (`E_OUTOFMEMORY`), which is why the runner reports such failures per build
instead of aborting.

New results are welcome: open an issue with the JSON from `--json` (it contains the adapter name,
vendor/device IDs, driver and DXC versions, and no other data from your machine).

## What it does not cover

f64, compute shaders, other shader models, and the driver's `f32tof16` conversion path. A
`preserved` result describes these specific draws on that driver, not a guarantee for every
instruction.

## Credits and history

The probe was written for the denormal-mode discussion in
[Xbox2Windows #312](https://github.com/R0Y00/Xbox2Windows/issues/312), where it first ran through
that project's renderer. This repository replaces that renderer with a minimal D3D12 runner of
its own; it contains no code from Xbox2Windows.

Made by Roan System Labs. AI-assisted: the code and text were prepared with an AI coding agent
and the measurements were run on real hardware; results from other people are linked to their
own reports.

## License

MIT, see [LICENSE](LICENSE).
