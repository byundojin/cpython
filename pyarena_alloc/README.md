# pyarena_alloc

PyArena Unit 3 — per-interpreter 메모리 쿼터를 위한 C 확장.

## 역할

`PyMem_SetAllocator` 로 Object 도메인에 훅을 걸어 subinterpreter 별 hard
memory limit 을 강제한다. 측정(`tracemalloc`)이 아니라 **할당 시점 예방** —
`"a" * 10**9` 같은 단일 거대 할당이 NULL 반환으로 거부되고 Python 이 자동으로
`MemoryError` 를 raise.

## 리포 경계

- 이 패키지의 C 소스는 PyArena 리포가 아닌 **CPython fork 체크아웃 안**
  (`/workspaces/PyArena/CPython/pyarena_alloc/`) 에 존재.
- CPython fork 의 로컬 `.git/info/exclude` 에 `/pyarena_alloc/` 등록 →
  CPython fork git 에도 untracked. 양쪽 리포 커밋 히스토리 오염 0.
- PyArena 리포는 `CPython/` 을 통째 `.gitignore` 하므로 이쪽 트리에 대한
  참조/변경 경로 없음.
- 두 리포 간 계약은 **설치된 Python 모듈 이름과 API** 뿐: `import pyarena_alloc`.

## 빌드 / 설치

```
cd /workspaces/PyArena/CPython/pyarena_alloc
/usr/local/bin/python -m pip install -e .
```

`pip install -e .` 는 editable 설치 — 소스 수정 후 재빌드만 하면 (`python
setup.py build_ext --inplace` 또는 `pip install -e . --force-reinstall`)
Python import 쪽이 즉시 반영.

Python 3.14 헤더는 일반 `/usr/local/include/python3.14/` 가 아니라 빌드
소스 트리(`../Include`, `..`) 에 있어서 `setup.py` 가 경로 두 개를
명시한다.

## Python API (완성 시점)

```python
import pyarena_alloc

pyarena_alloc.install()                       # 훅 설치. 멱등. subinterp 생성 전.
pyarena_alloc.set_quota(interp_id, bytes)     # 쿼터 설정
pyarena_alloc.clear_quota(interp_id)          # 해제 (이후 면제)
pyarena_alloc.get_used(interp_id) -> int      # 현재 사용량
pyarena_alloc.get_peak(interp_id) -> int      # 피크 사용량
```

## Checkpoint 진행 상태

- **CP1 (현재)** — 빈 skeleton. 모듈 로드만 OK, 기능 없음.
- CP2 — 쿼터 테이블 + 바인딩 (host 데이터만, 훅 미설치)
- CP3 — Object 도메인 훅 함수 + install()
- CP4 — CAS 쿼터 체크 + MemoryError 경로

설계 근거는 PyArena 리포의 `docs/pyarena_기술_선택과_이유.md` 참조.
