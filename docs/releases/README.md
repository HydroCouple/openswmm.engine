# Releasing OpenSWMM Engine

OpenSWMM Engine, developed by HydroCouple, ships as engine archives on GitHub Releases and as two Python packages on PyPI: `openswmm` (the bindings with the engine built in) and `openswmm-gpu-omp` (the OpenMP compute plugin, pinned to the same `openswmm` version). The `Deployment` workflow (`.github/workflows/deployment.yml`) builds and publishes all of them.

## The version

`CMakeLists.txt` is the source: `project(openswmm VERSION X.Y.Z)` and `OPENSWMM_PRERELEASE` (`alpha.N`, `beta.N`, `rc.N`, or empty for a final release). Nine files declare the version in thirteen places. Set them all with:

```sh
python scripts/bump_version.py 6.0.0-alpha.5
```

The script writes the engine's semver form (`6.0.0-alpha.5`) and the Python packages' PEP 440 form (`6.0.0a5`), then runs `scripts/check_version_sync.py`. CI runs the same gate and fails when any declaration is out of step or written twice. The two engine components report the version the build generates (`OPENSWMM_VERSION_FULL`).

`OPENSWMM_PRERELEASE` is a CMake cache variable. An existing build tree keeps reporting the old version until it is reconfigured with `-DOPENSWMM_PRERELEASE=<new>` or a fresh cache.

The committed Python version never carries `.devN` or `.postN`. CI adds those when it builds development wheels or a Python-only release (`scripts/python_release_version.py`).

## Releasing the engine: a `v` tag

1. Run `scripts/bump_version.py` with the new version.
2. Add `docs/releases/RELEASE_NOTES_<version>.md`.
3. In `CHANGELOG.md`, rename `## [Unreleased]` to the version and date, and open an empty `## [Unreleased]`.
4. Merge, then tag the merged commit and push the tag:

   ```sh
   git tag v6.0.0-alpha.5 && git push origin v6.0.0-alpha.5
   ```

The tag must equal the version in `CMakeLists.txt`; otherwise the run stops before building anything.

The run builds:
- the engine archives for four platforms;
- the wheels for Python 3.10 to 3.13 on five platforms;
- the companion wheels and the sdist.

It also runs the C++ unit-test gate and installs the wheels into fresh environments. When every check passes, it creates the GitHub Release and then publishes both packages to PyPI.

## Releasing the Python packages alone: a `py-v` tag

A `py-v` tag publishes the Python packages without an engine release. Use it for a bindings fix, or to put a development version on PyPI for a package that installs from there.

The tag is the engine version's PEP 440 form followed by `.postN`, `.devN` or both:

```sh
git tag py-v6.0.0a5.post1 && git push origin py-v6.0.0a5.post1   # a fix after 6.0.0a5
git tag py-v6.0.0a5.dev3  && git push origin py-v6.0.0a5.dev3    # a preview before it
```

The run builds every wheel and the sdist at that version from the tagged commit, which includes the engine as it is at that commit. It runs the wheel smoke tests and publishes to PyPI. It builds no engine archives and creates no GitHub Release. A tag for another engine version, or one without a suffix, is refused.

## Development wheels

Run the `Deployment` workflow by hand on `swmm6_rel`, from the Actions tab or with:

```sh
gh workflow run deployment.yml --ref swmm6_rel
```

The run builds wheels for every interpreter at `<version>.dev<run number>`. When every platform passes the smoke test, it replaces the wheels on the rolling `dev` pre-release and moves the `dev` tag to the commit it built. Only the latest build is kept. A full set takes two to two and a half hours. Pushes come faster than that and would cancel it, so the release pull request's own runs build Python 3.12 only; pushes don't cancel a manual run. Install the wheels with:

```sh
pip install --pre --find-links https://github.com/HydroCouple/openswmm.engine/releases/expanded_assets/dev openswmm openswmm-gpu-omp
```

These wheels are not on PyPI: a full set is about 475 MB, and PyPI limits a project to 10 GB.

## One-time setup

PyPI accepts uploads from this workflow through trusted publishing; no token is stored in the repository. On pypi.org, add a GitHub publisher to **both** `openswmm` and `openswmm-gpu-omp` (Manage → Publishing) with:

| Field | Value |
| --- | --- |
| Owner | `HydroCouple` |
| Repository | `openswmm.engine` |
| Workflow | `deployment.yml` |
| Environment | `pypi` |

In the repository's settings, create the `pypi` environment. Adding required reviewers to it makes every PyPI upload wait for an approval.
