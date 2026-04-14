"""Build script for the pyarena_alloc C extension.

이 패키지는 PyArena 프로젝트의 Unit 3 — per-interpreter 메모리 쿼터 —
을 구현하는 C 확장이다. `PyMem_SetAllocator` 로 Object 도메인에 훅을 걸어
subinterpreter 별 hard memory limit 을 강제한다.

리포 분리 경계:
    - 이 C 소스는 PyArena 리포가 아니라 CPython fork 체크아웃 (/workspaces/
      PyArena/CPython/) 안에 위치. 로컬 .git/info/exclude 에 등록되어 어떤
      git 의 추적도 받지 않음.
    - PyArena 쪽은 빌드 결과물만 `import pyarena_alloc` 으로 접근.

헤더 경로:
    Python 3.14 정식 install 이 없어서 (`/usr/local/include/python3.14` 없음),
    CPython 소스 트리의 Include/ + 최상위 디렉터리(pyconfig.h 위치)를 직접
    include path 로 넘긴다.
"""

from setuptools import Extension, setup

CPYTHON_ROOT = "/workspaces/PyArena/CPython"

ext = Extension(
    "pyarena_alloc",
    sources=["pyarena_alloc.c"],
    include_dirs=[
        f"{CPYTHON_ROOT}/Include",  # Python.h
        CPYTHON_ROOT,               # pyconfig.h (Include/ 아닌 최상위에 있음)
    ],
    extra_compile_args=["-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"],
)

setup(ext_modules=[ext])
