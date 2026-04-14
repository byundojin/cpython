"""CP2 tests — quota table data layer (hook not yet installed).

이 테스트는 `pyarena_alloc` 의 host-side 쿼터 테이블이 set/clear/lookup
라운드트립을 올바르게 처리하는지만 확인한다. CP2 에서는 allocator hook
이 아직 설치되지 않으므로 `used` 와 `peak` 는 언제나 0 — CP3 에서
hook 이 붙으면 그쪽 테스트가 실제 카운터 증감까지 커버.

실행: `/usr/local/bin/python -m pytest tests/test_table.py -v` 를
   `/workspaces/PyArena/CPython/pyarena_alloc/` 디렉터리에서.
"""

import pytest

import pyarena_alloc as pa


@pytest.fixture(autouse=True)
def clean_table():
    """매 테스트 전 테이블에 남은 엔트리 전부 제거.

    `install` 은 멱등이라 여러 번 호출해도 안전. 테스트 간 상호 간섭을
    방지하기 위해 매번 깨끗한 상태에서 시작한다.
    """
    # 넓은 id 범위를 한 번씩 clear — 어떤 테스트가 어떤 id 를 썼는지
    # 미리 알 수 없으니 보수적으로 전 범위 초기화.
    for iid in range(-10, 10_000):
        pa.clear_quota(iid)
    yield
    for iid in range(-10, 10_000):
        pa.clear_quota(iid)


def test_install_idempotent() -> None:
    # install() 여러 번 호출해도 예외 없음. CP2 에서는 내부 flag 만 flip.
    pa.install()
    pa.install()
    pa.install()


def test_set_quota_then_get_used_zero() -> None:
    # CP2 에는 hook 이 없으므로 set 이후에도 used 는 0 유지.
    pa.set_quota(1, 1024 * 1024)
    assert pa.get_used(1) == 0
    assert pa.get_peak(1) == 0


def test_get_used_unknown_returns_zero() -> None:
    # 설정 안 된 interp id 조회 시 0 반환. 에러 아님 — 호스트는 이 경로로
    # "quota 가 있나?" 를 구분하지 않고, 없으면 0 으로 보는 게 면제의 의미.
    assert pa.get_used(12345) == 0
    assert pa.get_peak(12345) == 0


def test_clear_quota_is_silent_noop_when_absent() -> None:
    # 없는 id 에 clear 호출해도 예외 없음. 멱등성 보장.
    pa.clear_quota(55555)
    pa.clear_quota(55555)


def test_set_then_clear_roundtrip() -> None:
    pa.set_quota(7, 1 << 20)
    pa.clear_quota(7)
    # clear 후에는 0 (unknown 과 동일 의미).
    assert pa.get_used(7) == 0
    assert pa.get_peak(7) == 0


def test_set_quota_updates_existing() -> None:
    # 같은 id 에 두 번 set 해도 table full 에러 없이 limit 만 갱신되어야 함.
    # CP2 에서는 limit 자체를 read 할 API 가 없으니 "에러 안 난다" 로 검증.
    pa.set_quota(3, 1024)
    pa.set_quota(3, 2048)
    pa.set_quota(3, 4096)


def test_multiple_distinct_interps() -> None:
    # 서로 다른 id 는 독립 슬롯. used 가 모두 0 이고, 하나를 clear 해도
    # 다른 id 는 영향 없음.
    pa.set_quota(10, 1024)
    pa.set_quota(20, 2048)
    pa.set_quota(30, 4096)
    assert pa.get_used(10) == 0
    assert pa.get_used(20) == 0
    assert pa.get_used(30) == 0

    pa.clear_quota(20)
    # 20 은 사라졌지만 10/30 은 살아있음 (CP2 는 used 로만 검증 가능).
    assert pa.get_used(10) == 0
    assert pa.get_used(20) == 0  # clear 후에도 0 (unknown 과 구분 불가는 CP2 한계)
    assert pa.get_used(30) == 0


def test_table_full_raises_runtime_error() -> None:
    # max_entries 만큼 채우고 그 다음 set_quota → RuntimeError.
    n = pa.max_entries()
    for i in range(n):
        pa.set_quota(1000 + i, 1024)

    with pytest.raises(RuntimeError, match="quota table full"):
        pa.set_quota(9999, 1024)


def test_table_full_recovers_after_clear() -> None:
    # 꽉 찬 상태에서 하나 비우면 다시 set_quota 가능.
    n = pa.max_entries()
    for i in range(n):
        pa.set_quota(2000 + i, 1024)

    with pytest.raises(RuntimeError):
        pa.set_quota(7777, 1024)

    pa.clear_quota(2000)
    pa.set_quota(7777, 1024)  # 이번엔 성공
    pa.clear_quota(7777)


def test_set_quota_requires_two_args() -> None:
    with pytest.raises(TypeError):
        pa.set_quota(1)  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        pa.set_quota()  # type: ignore[call-arg]


def test_get_used_requires_one_arg() -> None:
    with pytest.raises(TypeError):
        pa.get_used()  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        pa.get_used(1, 2)  # type: ignore[call-arg]


def test_module_doc_not_empty() -> None:
    assert pa.__doc__
    assert "quota" in pa.__doc__.lower()
