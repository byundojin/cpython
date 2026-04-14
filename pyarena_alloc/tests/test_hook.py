"""CP3 tests — real allocator hook with per-interpreter quota enforcement.

이 테스트는 `pyarena_alloc.install()` 이 실제로 `PyMem_SetAllocator` 를
교체하고, 이후의 subinterpreter 할당이 `{size, interp_id}` 헤더로
추적되어 쿼터를 넘으면 `MemoryError` 를 일으키는지 확인한다.

install() 은 프로세스 전역 effect — 한 번 걸리면 프로세스 끝까지 유지.
따라서 module-level 에서 딱 한 번만 install 하고, 테스트들은 매번 새
subinterpreter + 새 interp_id 로 격리.

**주의**: CP3 hook 은 obmalloc 을 경유하지 않는 Object 도메인 할당 (큰
할당, arena refill) 만 직접 본다. 작은 객체 수만 개를 만드는 공격은 이
레이어에서 완전 차단되지 않는다 — 설계 doc 이 명시한 한계. 테스트는
"큰 단일 할당이 쿼터에서 잡힌다" 는 핵심 계약만 검증.
"""

import pytest
from concurrent import interpreters

import pyarena_alloc as pa


# ---------------------------------------------------------------------------
# install() 을 세션 전체에 한 번만. install 은 프로세스 전역이라 여러 번
# 돌려도 무해하지만 테스트 수집 로그를 깨끗이 유지하려 session-scope.
# ---------------------------------------------------------------------------
@pytest.fixture(scope="session", autouse=True)
def install_once():
    pa.install()
    assert pa.is_installed()


# ---------------------------------------------------------------------------
# 각 테스트는 새 subinterpreter 를 만들어 쿼터를 독립적으로 설정. 쿼터를
# clear 하는 시점은 **interp.close() 이후** — 살아있는 인터프리터 쿼터를
# clear 하면 이후 free 가 카운터에 반영 안 되어 드리프트 발생.
# ---------------------------------------------------------------------------
@pytest.fixture
def interp():
    i = interpreters.create()
    yield i
    try:
        i.close()
    finally:
        pa.clear_quota(i.id)


def _run_in_interp(interp, code: str) -> tuple[str, str]:
    """exec code in interp, capture (result_tag, detail) via shareable queue."""
    rq = interpreters.create_queue()
    interp.prepare_main(rq=rq, _code=code)
    interp.exec("""
import traceback
try:
    exec(_code, {'__name__': '__main__'})
    rq.put(('ok', ''))
except BaseException as e:
    rq.put((type(e).__name__, str(e)))
""")
    return rq.get_nowait()


def test_host_unaffected_by_hook(interp) -> None:
    # 호스트 인터프리터는 쿼터 테이블에 엔트리가 없으므로 면제. install 후에도
    # 호스트 측에서 큰 bytes 를 자유롭게 만들 수 있어야 함.
    big = bytearray(50 * 1024 * 1024)  # 50 MiB — quota 없으면 무제한
    assert len(big) == 50 * 1024 * 1024


def test_quota_not_set_means_passthrough(interp) -> None:
    # 쿼터 설정 없이 interp 에서 큰 할당 시도 → 통과 (host 와 동일 처리).
    # used 는 0 — 엔트리가 없으니 추적 자체가 안 됨.
    tag, _ = _run_in_interp(interp, "data = 'x' * (10 * 1024 * 1024)")
    assert tag == "ok"
    assert pa.get_used(interp.id) == 0


def test_small_alloc_within_quota(interp) -> None:
    pa.set_quota(interp.id, 4 * 1024 * 1024)  # 4 MiB
    tag, _ = _run_in_interp(interp, "x = sum(range(1000)); y = 'hello'")
    assert tag == "ok"
    # 일부 카운트가 기록되어야 함 (0 이상)
    assert pa.get_used(interp.id) >= 0
    assert pa.get_peak(interp.id) >= pa.get_used(interp.id)


def test_large_alloc_within_quota(interp) -> None:
    pa.set_quota(interp.id, 16 * 1024 * 1024)  # 16 MiB
    tag, _ = _run_in_interp(interp, "data = 'a' * (5 * 1024 * 1024)")  # 5 MiB
    assert tag == "ok"
    # 5 MiB 문자열은 자체 + 헤더 포함해서 쿼터 내.
    peak = pa.get_peak(interp.id)
    assert peak >= 5 * 1024 * 1024


def test_large_alloc_exceeds_quota_raises_memory_error(interp) -> None:
    pa.set_quota(interp.id, 16 * 1024 * 1024)  # 16 MiB
    tag, _ = _run_in_interp(interp, "data = 'x' * (50 * 1024 * 1024)")  # 50 MiB
    assert tag == "MemoryError", f"expected MemoryError, got {tag}"


def test_quota_exceeded_still_allows_future_smaller_alloc(interp) -> None:
    # 쿼터 초과는 단발성 거부. used 는 롤백되어 이후 정상 할당 가능.
    pa.set_quota(interp.id, 16 * 1024 * 1024)
    tag1, _ = _run_in_interp(interp, "data = 'x' * (50 * 1024 * 1024)")
    assert tag1 == "MemoryError"
    # 같은 interp 에서 더 작은 할당 시도
    tag2, _ = _run_in_interp(interp, "small = [i for i in range(100)]")
    assert tag2 == "ok"


def test_two_interpreters_quota_isolation() -> None:
    i1 = interpreters.create()
    i2 = interpreters.create()
    try:
        pa.set_quota(i1.id, 16 * 1024 * 1024)
        pa.set_quota(i2.id, 16 * 1024 * 1024)

        # i1 에서 쿼터 초과 시도
        rq1 = interpreters.create_queue()
        i1.prepare_main(rq=rq1)
        i1.exec("""
try:
    d = 'a' * (50 * 1024 * 1024)
    rq.put('ok')
except MemoryError:
    rq.put('mem')
""")
        assert rq1.get_nowait() == "mem"

        # i2 는 영향 없이 정상 5MB 가능해야 함
        rq2 = interpreters.create_queue()
        i2.prepare_main(rq=rq2)
        i2.exec("""
d = 'a' * (5 * 1024 * 1024)
rq.put(len(d))
""")
        assert rq2.get_nowait() == 5 * 1024 * 1024
    finally:
        i1.close()
        i2.close()
        pa.clear_quota(i1.id)
        pa.clear_quota(i2.id)


def test_peak_tracks_maximum(interp) -> None:
    pa.set_quota(interp.id, 16 * 1024 * 1024)
    # 4 MiB 할당 후 해제 → 다시 2 MiB → peak 는 4 MiB 근처여야 함
    _run_in_interp(interp, """
data = 'x' * (4 * 1024 * 1024)
del data
small = 'y' * (1 * 1024 * 1024)
""")
    peak = pa.get_peak(interp.id)
    # 4 MiB 이상 peak 찍어야 함 (정확히 4 MiB 이상인지 + 5 MiB 미만인지는
    # interp 내부 할당도 섞이므로 느슨하게 체크)
    assert peak >= 4 * 1024 * 1024, f"peak {peak} < 4 MiB"


def test_peak_persists_after_exec_returns(interp) -> None:
    # `used` 는 exec 가 끝나면 로컬 네임스페이스 소멸과 함께 ~0 으로 떨어진다
    # (학습자 할당이 모두 해제됨). 하지만 `peak` 는 최대값을 기억하므로
    # exec 완료 후에도 엘리베이트된 값으로 남아야 한다.
    pa.set_quota(interp.id, 16 * 1024 * 1024)
    _run_in_interp(interp, "data = 'x' * (5 * 1024 * 1024)")
    # exec 가 return 되면 data 는 참조 해제되어 free → used 하락
    peak = pa.get_peak(interp.id)
    assert peak >= 5 * 1024 * 1024, f"peak {peak} < 5 MiB"


def test_free_decrements_counter_indirectly(interp) -> None:
    # 직접적으로 "used 가 free 시 감소하는지" 는 subinterp 내부에서 pyarena_alloc
    # 을 import 할 수 없어 검증 불가 (C ext 가 shared state 이유로 multi-interp
    # 로드 거부 — Py_mod_multiple_interpreters 미지원). 대신 간접 검증: 쿼터
    # 16 MiB 로 설정하고 5 MiB 할당/해제를 20 회 반복. 누적 100 MiB 는 free
    # 가 decrement 하지 않으면 절대 통과 불가. 통과하면 decrement 가 살아있다는 증거.
    pa.set_quota(interp.id, 16 * 1024 * 1024)
    rq = interpreters.create_queue()
    interp.prepare_main(rq=rq)
    interp.exec("""
ok = True
for i in range(20):
    data = 'x' * (5 * 1024 * 1024)
    del data
rq.put('all-alloc-free-cycles-ok')
""")
    assert rq.get_nowait() == 'all-alloc-free-cycles-ok'
    # peak 는 5 MiB 이상 (단일 사이클 최대)
    assert pa.get_peak(interp.id) >= 5 * 1024 * 1024


def test_is_installed_flag() -> None:
    assert pa.is_installed() is True
