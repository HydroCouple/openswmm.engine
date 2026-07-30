---
project: SWMM6_2
type: current_state
updated: 2026-05-24
---

## State
Phase 1 (Mac Intel) is COMPLETE. SWMMVis.app (45 MB) and SWMMVis-6.0.0-alpha.1-Darwin.dmg (57 MB) built successfully at ~/Projects/openswmm.gui/build/darwin/. Five bugs were fixed along the way, all due to macOS 26 beta + CMake 4.x incompatibilities. Phase 2 (Ubuntu VM) is next.

## Active
- Ready to start Phase 2: Ubuntu VM build

## Next
1. Phase 2: Ubuntu VM — follow build_plan.md Linux steps
2. Phase 3: Windows 11 VM — follow build_plan.md Windows steps  
3. PR upstream fixes to openswmm.engine (SQLite static lib install check)
4. PR upstream fixes to openswmm.gui (wmslayer.cpp Qt6 string comparison)
