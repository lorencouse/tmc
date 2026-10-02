"""Toolchain discovery shared by the tools/*regression_test.py suites.

Header locations resolve in order: an explicit environment variable
(TMC_SDL3_INCLUDE / TMC_JSON_INCLUDE), pkg-config, then the xmake package
cache. When none match, no flag is added and the compiler's default search
path decides.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys

# Apple ld has no --gc-sections; -dead_strip is its equivalent.
GC_SECTIONS = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'


def cc():
    return shlex.split(os.environ.get('CC', 'cc'))


def cxx():
    return shlex.split(os.environ.get('CXX', 'c++'))


def _pkg_config_cflags(name):
    try:
        output = subprocess.check_output(['pkg-config', '--cflags', name], text=True,
                                         stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return None
    return shlex.split(output)


def _xmake_include(package, header):
    root = Path(os.environ.get('XMAKE_GLOBALDIR', Path.home())) / '.xmake/packages'
    headers = sorted(root.glob(f'{package[0]}/{package}/*/*/include/{header}'))
    if not headers:
        return None
    return ['-I' + str(headers[-1].parents[len(Path(header).parts) - 1])]


def _include_flags(env_var, pkg_name, xmake_package, header):
    if os.environ.get(env_var):
        return ['-I' + os.environ[env_var]]
    flags = _pkg_config_cflags(pkg_name)
    if flags is not None:
        return flags
    return _xmake_include(xmake_package, header) or []


def sdl3_cflags():
    return _include_flags('TMC_SDL3_INCLUDE', 'sdl3', 'libsdl3', 'SDL3/SDL.h')


def json_cflags():
    return _include_flags('TMC_JSON_INCLUDE', 'nlohmann_json', 'nlohmann_json', 'nlohmann/json.hpp')
