---
project: SWMM6_2
type: build_plan
updated: 2026-05-23
---

## Build Plan: OpenSWMM GUI — 3 Platforms

### Phase 1 — Mac (Intel x86_64) · local machine
Mac already has: Xcode CLT (clang 21), CMake 4.3.2, Ninja 1.13, Git 2.54, Homebrew, Python 3.12

**Missing:** Qt 6.7, vcpkg

```bash
# Step 1 — Install Qt 6.7 via aqtinstall
pip3 install aqtinstall
aqt install-qt mac desktop 6.7.3 clang_64 -m qtcharts -O ~/Qt
# Qt lands at ~/Qt/6.7.3/macos

# Step 2 — Clone and bootstrap vcpkg
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# Step 3 — Clone GUI repo
git clone -b swmm6_gui https://github.com/HydroCouple/openswmm.gui.git ~/Projects/openswmm.gui
cd ~/Projects/openswmm.gui

# Step 4 — Set environment variables
export VCPKG_ROOT=~/vcpkg
export QT_ROOT_DIR=~/Qt/6.7.3/macos

# Step 5 — Configure (vcpkg compiles deps ~45 min first run)
cmake --preset Darwin

# Step 6 — Build
cmake --build --preset Darwin

# Step 7 — Verify
ls packages/   # should contain a .dmg
```

**Success signal:** `.dmg` file in `packages/`

---

### Phase 2 — Ubuntu VM

```bash
# Step 1 — System packages
sudo apt update
sudo apt install -y build-essential cmake ninja-build git curl zip unzip tar \
  pkg-config libgl1-mesa-dev python3-pip

# Step 2 — Qt 6.7
pip3 install aqtinstall
aqt install-qt linux desktop 6.7.3 gcc_64 -m qtcharts -O ~/Qt

# Step 3 — vcpkg
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# Step 4 — Clone GUI repo
git clone -b swmm6_gui https://github.com/HydroCouple/openswmm.gui.git ~/openswmm.gui
cd ~/openswmm.gui

# Step 5 — Environment
export VCPKG_ROOT=~/vcpkg
export QT_ROOT_DIR=~/Qt/6.7.3/gcc_64

# Step 6 — Configure + Build
cmake --preset Linux
cmake --build --preset Linux

# Step 7 — Verify
ls packages/   # .tar.gz and/or AppImage
```

---

### Phase 3 — Windows 11 VM

```
# Step 1 — Install Visual Studio 2022 Community
#   Workload: "Desktop development with C++"
#   Ensures MSVC compiler + Windows SDK

# Step 2 — Install tools
#   CMake: cmake.org/download (add to PATH)
#   Git: git-scm.com
#   Python: python.org (add to PATH)
#   Ninja: via pip or scoop

# Step 3 — Qt 6.7 (run in PowerShell)
pip install aqtinstall
aqt install-qt windows desktop 6.7.3 win64_msvc2019_64 -m qtcharts -O C:\Qt

# Step 4 — vcpkg
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# Step 5 — Clone GUI repo
git clone -b swmm6_gui https://github.com/HydroCouple/openswmm.gui.git C:\openswmm.gui
cd C:\openswmm.gui

# Step 6 — Environment (PowerShell)
$env:VCPKG_ROOT = "C:\vcpkg"
$env:QT_ROOT_DIR = "C:\Qt\6.7.3\msvc2019_64"

# Step 7 — Configure + Build (from VS 2022 Developer PowerShell)
cmake --preset Windows
cmake --build --preset Windows

# Step 8 — Verify
dir packages\   # .exe NSIS installer
```

---

## Key Known Facts
- vcpkg baseline pinned to d5ec528843d29e3a52d745a64b469f810b2cedbf (engine vcpkg.json)
- GUI deps: GDAL (jpeg+png+sqlite3), gtest, nanoflann, sundials, hdf5, openssl
- Engine deps: sundials, hdf5, gtest (auto-fetched by GUI CMake from develop branch)
- Darwin preset uses QT_ROOT_DIR env var — must be set before cmake configure
- First vcpkg dep build: ~45 min; subsequent builds use cache
- Windows must use VS Developer shell for cmake --preset Windows to find MSVC
