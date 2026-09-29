<div align="center">

<img src="docs/rin.png" alt="Rin logo" width="128"/>

# Rin

[![CI](https://github.com/Linductor-alkaid/rin/actions/workflows/ci.yml/badge.svg)](https://github.com/Linductor-alkaid/rin/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Linductor-alkaid/rin)](https://github.com/Linductor-alkaid/rin/releases)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-%E2%89%A53.25-064F8C?logo=cmake&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux-f96854?logo=linux&logoColor=white)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

English | [简体中文](README_zh.md)

</div>

**Rin** is a real-time Intel RealSense workbench built with modern C++: live RGB/depth
preview, a 3D camera-pose view driven by IMU fusion, and a drag-and-drop node editor
for on-camera image processing — all on an executor-backed runtime with explicit
state machines and sanitizer-tested concurrency.

## Screenshots

| Preview | 3D pose view |
| --- | --- |
| ![Preview page](screenshots/m5-02/preview_page.png) | ![Pose page](screenshots/m5-02/pose_page.png) |
| **Workflow — graph running** | **Workflow — node output & parameters** |
| ![Workflow running](screenshots/m5-07/workflow_graph_running.png) | ![Node output](screenshots/m5-07/workflow_node_output_params.png) |

## Features

- **Live preview** — RGB + depth streaming with device selection, resolution switching,
  jet/grayscale depth palettes, and intrinsics display. Hot-plug friendly: the app starts
  without a camera (a `Waiting` steady state) and resumes automatically.
- **3D pose view** — camera pose in a fixed world frame (view frustum + ground grid),
  fused from accelerometer/gyroscope with a Mahony complementary filter (`ImuFuser`,
  Core pure logic). Six-axis yaw drift is disclosed as a sensor limitation.
- **Node-based image workflow** — build processing graphs by dragging nodes from the
  palette and wiring ports: crop, downscale, custom convolution kernel, Gaussian blur,
  histogram equalization, grayscale, FFT low/high/band-pass, plus depth-camera sources
  (colorized/grayscale/adaptive) and grayscale-domain variants. Instant validation,
  type-colored wiring, parameter hot-reload ("applies next frame"), per-node output
  thumbnails, and a performance panel (end-to-end FPS, per-node cost, explicit drop
  counters). Start/Stop run control with "pending — applies next frame" graph edits.
- **Executor-backed runtime** — every async task, blocking capture loop, and
  cross-thread channel runs on a pinned executor library; bounded queues, cooperative
  cancellation, explicit failure events, and a tested shutdown order. No hidden threads,
  no fire-and-forget work.
- **Tested like it matters** — state-machine, boundary, and integration tests across
  debug/ASan/UBSan/TSan presets, plus hardware-in-the-loop tests against a real
  D435if. Measured throughput with a typical 848×480 FFT chain: 30 fps camera rate,
  zero drops (~3.4× headroom) — see the
  [benchmark record](docs/benchmarks/workflow-throughput-848x480.md).

## Install (Linux, deb)

Download `rin_<version>_amd64.deb` from the
[Releases](https://github.com/Linductor-alkaid/rin/releases) page (CI produces it for
every `v*` tag, built in an Ubuntu 20.04 container):

```bash
sudo apt install ./rin_<version>_amd64.deb
rin
```

The deb is self-contained: it bundles the pinned librealsense2 runtime
(`/usr/lib/rin`) and RealSense udev rules (device access without root). No Intel
RealSense SDK installation required — plug in a camera and run. Runs on Ubuntu
20.04 (amd64) and newer; packaged fonts land under `/usr/share/rin/fonts/` so the
UI renders correctly on systems without the bundled dev-layout fonts.

## Build from source

Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler (GCC 10+; GCC 12/13 in CI), and
libusb-1.0, libudev, libcurl, OpenGL/X11 development packages. All third-party
libraries (librealsense2, executor, EUI-NEO, kissfft) are pinned to exact commits in
[`third_party/dependencies.lock`](third_party/dependencies.lock) and fetched/built
by CMake — nothing else to preinstall. The full CI dependency list lives in
[ci.yml](.github/workflows/ci.yml).

```bash
cmake --preset debug            # or release / asan / ubsan / tsan
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Hardware tests (label `hardware`) run against a connected RealSense camera and are
skipped when no device is present.

Build the deb package:

```bash
cmake --preset release
cmake --build build/release
cmake --build build/release --target package    # output in build/release/dist/
```

Options: `RIN_BUILD_TESTS` (default ON), `RIN_BUILD_VIEWER` (default ON),
`RIN_BUILD_TOOLS` (benchmark/measurement tools, default ON),
`RIN_ENABLE_ASAN/UBSAN/TSAN` (set by the sanitizer presets).

## Architecture

```
apps/viewer (rin)                 EUI-NEO front end — page navigation, node canvas,
                                  panels; executor lifecycle owner (AppRuntime)
src/adapters/realsense            librealsense2 adapter — blocking capture on an
  (rin_realsense_adapter)         executor blocking worker; executor::comm mailboxes
src/core (rin_core)               camera service contracts (ICameraService), explicit
                                  state machines, pixel conversion, IMU fusion,
                                  image nodes + workflow engine — no third-party types
third_party/executor, eui-neo,    pinned dependencies (configure-time commit check)
  librealsense2, kissfft
```

House rules that hold the layers apart: public headers never include third-party
types; adapters depend on core interfaces, never the reverse; all concurrency goes
through the executor's public capabilities and `executor::comm` channels are bounded;
every third-party dependency gets a feedback ledger
([`docs/executor_feedback/`](docs/executor_feedback/ledger.md),
[`docs/dependency_feedback/`](docs/dependency_feedback/README.md)).

## Documentation

- [Project standards](docs/project/project-standards.md) — planning, verification,
  commit/MR discipline
- [Implementation plan](docs/plans/rin-implementation-plan.md) and
  [milestone records](docs/plans/) with acceptance evidence
- [Decision records](docs/decisions/) (ADR-style, Chinese)
- [Design documents](docs/design/) — viewer design system, workbench information
  architecture, image-workflow semantics
- [Benchmarks](docs/benchmarks/) — measured workflow throughput

## Changelog

Per-release changes are tracked in [CHANGELOG.md](CHANGELOG.md) (Chinese).

## License

Rin is released under the [MIT License](LICENSE). Third-party dependencies keep
their own licenses — Apache-2.0 (librealsense2, executor, EUI-NEO) and
BSD-3-Clause (kissfft); see
[`third_party/dependencies.lock`](third_party/dependencies.lock) and the
[dependency ledgers](docs/dependency_feedback/README.md) for details.
