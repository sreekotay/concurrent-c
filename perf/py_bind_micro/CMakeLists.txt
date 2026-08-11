# Nanobind half of perf/py_bind_micro — driven by run.py.
cmake_minimum_required(VERSION 3.18...3.30)
project(func_nanobind LANGUAGES CXX)

find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
find_package(nanobind CONFIG REQUIRED)

# Size-optimized bindings (nanobind default / their [opt] column).
nanobind_add_module(func_nanobind generated/func_nanobind.cpp)
