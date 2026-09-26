"""Copy Python modules/stubs into a CMake install prefix for package testing.

CMake installs native modules; scikit-build normally adds these source files
when producing a wheel. This helper supplies the same files for feature builds.
"""
from pathlib import Path
import shutil
import sys


def main():
    source = Path(__file__).resolve().parents[1] / 'openswmm'
    destination = Path(sys.argv[1]).resolve() / 'openswmm'
    for path in source.rglob('*'):
        if path.is_file() and (path.suffix in ('.py', '.pyi') or path.name == 'py.typed'):
            target = destination / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


if __name__ == '__main__':
    main()
