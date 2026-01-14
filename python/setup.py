# Description: This file is used to build the python package for the openswmmcore package.
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

# python imports
import os
import sys
import platform
import subprocess
from setuptools import Command, find_packages
from setuptools.command.build_ext import build_ext
from setuptools.command.build_py import build_py
import shutil
import re

# third party imports
from skbuild import setup

# local imports

# ======================================================================================================================

# Check if we are in debug mode
OPENSWMM_DEBUG_MODE = os.getenv('DEBUG', 'False').lower() in ('true', '1', 't')

# Get the platform system
platform_system = platform.system()

# Get the directory containing this file
here = os.path.abspath(os.path.dirname(__file__))


def debug_qualifier():
    """
    Get the debug qualifier
    """
    if OPENSWMM_DEBUG_MODE:
        return "-debug"
    else:
        return ""


def get_readme():
    """
    Get readme from toolkit
    """
    # Read the README file
    # Copy to build folder first
    if not os.path.exists(os.path.join(here, "README.md")):
        shutil.copyfile(os.path.join(here, r"./../README.md"), os.path.join(here, "README.md"))

    # Read the README file
    with open(os.path.join(here, "README.md"), encoding="utf-8") as f:
        long_description = f.read()

    return long_description


def get_cmake_args():
    """
    Get cmake arguments
    :return:
    """
    # Get the cmake arguments
    cmake_args = os.getenv(
        "OPENSWMM_CMAKE_ARGS", [f"--preset={platform_system}{debug_qualifier()}"]
    )

    return cmake_args

# def get_version(qualifier: str = 'dev0'):
#     """Extract version from version.h file"""
#     version_file = os.path.join(here, "../solver/version.h")
#     with open(version_file, 'r') as f:
#         content = f.read()
#         major = re.search(r'#define OPENSWMM_VERSION_MAJOR (\d+)', content).group(1)
#         minor = re.search(r'#define OPENSWMM_VERSION_MINOR (\d+)', content).group(1)
#         patch = re.search(r'#define OPENSWMM_VERSION_PATCH (\d+)', content).group(1)
#
#     version = f"{major}.{minor}.{patch}"
#
#     if qualifier:
#         version += f".{qualifier}"
#
#     return version

setup(
    name="openswmm",
    long_description=get_readme(),
    long_description_content_type="text/markdown",
    packages=find_packages(),
    version="5.3.0.dev1",
    cmake_args=[
        *get_cmake_args(),
    ],
    include_package_data=True,
)
