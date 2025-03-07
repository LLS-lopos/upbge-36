#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2010-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Module for accessing project file data for Blender.

Before use, call init(cmake_build_dir).
"""

# TODO: Use CMAKE_EXPORT_COMPILE_COMMANDS (compile_commands.json)
# Instead of Eclipse project format.

__all__ = (
    "SIMPLE_PROJECTFILE",
    "SOURCE_DIR",
    "CMAKE_DIR",
    "PROJECT_DIR",
    "source_list",
    "is_project_file",
    "is_c_header",
    "is_py",
    "cmake_advanced_info",
    "cmake_compiler_defines",
    "project_name_get",
    "init",
)

from typing import (
    List,
    Tuple,
    Union,
    # Proxies for `collections.abc`
    Callable,
    Iterator,
)


import sys
if sys.version_info.major < 3:
    print("\nPython3.x needed, found %s.\nAborting!\n" %
          sys.version.partition(" ")[0])
    sys.exit(1)


import subprocess
import os
from os.path import (
    abspath,
    dirname,
    exists,
    join,
    normpath,
    splitext,
)

SOURCE_DIR = join(dirname(__file__), "..", "..")
SOURCE_DIR = normpath(SOURCE_DIR)
SOURCE_DIR = abspath(SOURCE_DIR)

SIMPLE_PROJECTFILE = False

# must initialize from 'init'
CMAKE_DIR = ""
PROJECT_DIR = ""


def init(cmake_path: str) -> bool:
    global CMAKE_DIR, PROJECT_DIR

    # get cmake path
    cmake_path = cmake_path or ""

    if (not cmake_path) or (not exists(join(cmake_path, "CMakeCache.txt"))):
        cmake_path = os.getcwd()
    if not exists(join(cmake_path, "CMakeCache.txt")):
        print("CMakeCache.txt not found in %r or %r\n"
              "    Pass CMake build dir as an argument, or run from that dir, aborting" %
              (cmake_path, os.getcwd()))
        return False

    PROJECT_DIR = CMAKE_DIR = cmake_path
    return True


def source_list(
        path: str,
        filename_check: Union[Callable[[str], bool], None] = None,
) -> Iterator[str]:
    for dirpath, dirnames, filenames in os.walk(path):
        # skip '.git'
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]

        for filename in filenames:
            filepath = join(dirpath, filename)
            if filename_check is None or filename_check(filepath):
                yield filepath


# extension checking
def is_cmake(filename: str) -> bool:
    ext = splitext(filename)[1]
    return (ext == ".cmake") or (filename.endswith("CMakeLists.txt"))


def is_c_header(filename: str) -> bool:
    ext = splitext(filename)[1]
    return (ext in {".h", ".hpp", ".hxx", ".hh"})


def is_py(filename: str) -> bool:
    ext = splitext(filename)[1]
    return (ext == ".py")


def is_glsl(filename: str) -> bool:
    ext = splitext(filename)[1]
    return (ext == ".glsl")


def is_c(filename: str) -> bool:
    ext = splitext(filename)[1]
    return (ext in {".c", ".cpp", ".cxx", ".m", ".mm", ".rc", ".cc", ".inl", ".osl"})


def is_c_any(filename: str) -> bool:
    return is_c(filename) or is_c_header(filename)


def is_project_file(filename: str) -> bool:
    return (is_c_any(filename) or is_cmake(filename) or is_glsl(filename))


def cmake_advanced_info() -> (
        Union[Tuple[List[str], List[Tuple[str, str]]],
              Tuple[None, None]]
):
    """ Extract includes and defines from cmake.
    """

    make_exe = cmake_cache_var("CMAKE_MAKE_PROGRAM")
    if make_exe is None:
        print("Make command not found: CMAKE_MAKE_PROGRAM")
        return None, None

    make_exe_basename = os.path.basename(make_exe)

    def create_eclipse_project() -> str:
        print("CMAKE_DIR %r" % CMAKE_DIR)
        if sys.platform == "win32":
            raise Exception("Error: win32 is not supported")
        else:
            if make_exe_basename.startswith(("make", "gmake")):
                cmd = ("cmake", CMAKE_DIR, "-GEclipse CDT4 - Unix Makefiles")
            elif make_exe_basename.startswith("ninja"):
                cmd = ("cmake", CMAKE_DIR, "-GEclipse CDT4 - Ninja")
            else:
                raise Exception("Unknown make program %r" % make_exe)

        subprocess.check_call(cmd)
        return join(CMAKE_DIR, ".cproject")

    includes = []
    defines = []

    project_path = create_eclipse_project()

    if not exists(project_path):
        print("Generating Eclipse Project File Failed: %r not found" % project_path)
        return None, None

    from xml.dom.minidom import parse
    tree = parse(project_path)

    # to check on nicer xml
    # f = open(".cproject_pretty", 'w')
    # f.write(tree.toprettyxml(indent="    ", newl=""))

    ELEMENT_NODE = tree.ELEMENT_NODE

    cproject, = tree.getElementsByTagName("cproject")
    for storage in cproject.childNodes:
        if storage.nodeType != ELEMENT_NODE:
            continue

        if storage.attributes["moduleId"].value == "org.eclipse.cdt.core.settings":
            cconfig = storage.getElementsByTagName("cconfiguration")[0]
            for substorage in cconfig.childNodes:
                if substorage.nodeType != ELEMENT_NODE:
                    continue

                moduleId = substorage.attributes["moduleId"].value

                # org.eclipse.cdt.core.settings
                # org.eclipse.cdt.core.language.mapping
                # org.eclipse.cdt.core.externalSettings
                # org.eclipse.cdt.core.pathentry
                # org.eclipse.cdt.make.core.buildtargets

                if moduleId == "org.eclipse.cdt.core.pathentry":
                    for path in substorage.childNodes:
                        if path.nodeType != ELEMENT_NODE:
                            continue
                        kind = path.attributes["kind"].value

                        if kind == "mac":
                            # <pathentry kind="mac" name="PREFIX" path="" value="&quot;/opt/blender25&quot;"/>
                            defines.append((path.attributes["name"].value, path.attributes["value"].value))
                        elif kind == "inc":
                            # <pathentry include="/data/src/blender/blender/source/blender/editors/include" kind="inc" path="" system="true"/>
                            includes.append(path.attributes["include"].value)
                        else:
                            pass

    return includes, defines


def cmake_cache_var(var: str) -> Union[str, None]:
    def l_strip_gen(cache_file):
        for l in cache_file:
            yield l.strip()

    with open(os.path.join(CMAKE_DIR, "CMakeCache.txt"), encoding='utf-8') as cache_file:
        lines = [
            l_strip for l_strip in l_strip_gen(cache_file)
            if l_strip and not l_strip.startswith(("//", "#"))
        ]

    for l in lines:
        if l.split(":")[0] == var:
            return l.split("=", 1)[-1]
    return None


def cmake_compiler_defines() -> Union[List[str], None]:
    compiler = cmake_cache_var("CMAKE_C_COMPILER")  # could do CXX too

    if compiler is None:
        print("Couldn't find the compiler, os defines will be omitted...")
        return None

    import tempfile
    temp_c = tempfile.mkstemp(suffix=".c")[1]
    temp_def = tempfile.mkstemp(suffix=".def")[1]

    os.system("%s -dM -E %s > %s" % (compiler, temp_c, temp_def))

    temp_def_file = open(temp_def)
    lines = [l.strip() for l in temp_def_file if l.strip()]
    temp_def_file.close()

    os.remove(temp_c)
    os.remove(temp_def)
    return lines


def project_name_get() -> Union[str, None]:
    return cmake_cache_var("CMAKE_PROJECT_NAME")
