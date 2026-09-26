"""Lifetime and native-call coordination shared by engine views.

Independent engines may advance concurrently. Access to one engine from a
second thread during a native call fails promptly instead of racing its state
or deadlocking a callback waiting for that thread. Callbacks on the advancing
thread may inspect state and apply supported forcing; lifecycle reentry is
refused by the owner. Raw pointer compatibility only accepts registered owners.
"""
from contextlib import contextmanager
from threading import RLock
from weakref import WeakValueDictionary
from typing import Any, Callable, Iterator

from ._enums import ErrorCode
from ._exceptions import BadHandleError, LifecycleError, StaleObjectError

_owners: WeakValueDictionary[int, Any] = WeakValueDictionary()


def register_owner(owner: Any, address: int) -> int:
    if address:
        _owners[address] = owner
    return address


def resolve_owner(owner: Any) -> Any:
    if isinstance(owner, int):
        import warnings
        warnings.warn("Pass the Solver to Surface2D instead of solver.handle",
                      DeprecationWarning, stacklevel=3)
        address = owner
        try:
            owner = _owners[address]
        except KeyError:
            raise BadHandleError(ErrorCode.BADHANDLE,
                                 "Pointer has no live Python engine owner") from None
        if owner.handle != address:
            raise BadHandleError(ErrorCode.BADHANDLE, "Pointer no longer belongs to its owner")
    if not hasattr(owner, "handle") or not owner.handle:
        raise BadHandleError(ErrorCode.BADHANDLE, "Engine has been destroyed")
    return owner


class NativeAccess:
    """Per-owner native operation guard and deferred callback exception."""

    def __init__(self) -> None:
        self.lock = RLock()
        self.active = 0
        self.forbid_access = False
        self.error: BaseException | None = None

    def check(self) -> None:
        if self.forbid_access:
            raise LifecycleError(ErrorCode.LIFECYCLE, "Engine access is forbidden inside a staging mapper")
        if not self.lock.acquire(blocking=False):
            raise LifecycleError(ErrorCode.LIFECYCLE,
                                 "Engine is in use by another thread")
        self.lock.release()

    def require_idle(self) -> None:
        self.check()
        if self.active:
            raise LifecycleError(ErrorCode.LIFECYCLE,
                                 "Cannot change lifecycle during a native call or callback")

    @contextmanager
    def operation(self, owner: Any, expected: int = 0) -> Iterator[None]:
        if not self.lock.acquire(blocking=False):
            raise LifecycleError(ErrorCode.LIFECYCLE,
                                 "Engine is in use by another thread")
        try:
            address = owner.handle
            if not address:
                raise BadHandleError(ErrorCode.BADHANDLE, "Engine has been destroyed")
            if expected and address != expected:
                raise StaleObjectError("Engine handle changed")
            self.active += 1
            try:
                yield
            finally:
                self.active -= 1
        finally:
            self.lock.release()

    def raise_callback_error(self) -> None:
        error, self.error = self.error, None
        if error is not None:
            raise error


class Callback:
    """Capture Python failures without unwinding through a C callback ABI.

The first failure is re-raised after the native operation returns. Further
callbacks in that operation are suppressed; its current native step may finish.
"""
    def __init__(self, access: NativeAccess, callback: Callable[..., Any]) -> None:
        if not callable(callback):
            raise TypeError("callback must be callable or None")
        self.access = access
        self.callback = callback

    def __call__(self, *args: Any) -> None:
        if self.access.error is None:
            try:
                self.callback(*args)
            except BaseException as error:
                self.access.error = error


class EngineView:
    """An owner-backed domain view invalidated by lifecycle/structural changes."""

    def __init__(self, owner: Any) -> None:
        self._owner = resolve_owner(owner)
        self._generation = self._owner.generation

    def _address(self) -> int:
        if self._generation != self._owner.generation:
            raise StaleObjectError("View was invalidated; reacquire it from the solver")
        address: int = self._owner.handle
        if not address:
            raise BadHandleError(ErrorCode.BADHANDLE, "Engine has been destroyed")
        return address


class StageMapper:
    """Own callback result storage and defer exceptions across the C ABI."""

    def __init__(self, access: NativeAccess, callback: Callable[..., Any]) -> None:
        if not callable(callback):
            raise TypeError("mapper must be callable")
        self.access = access
        self.callback = callback
        self.storage = b""
        self.error: BaseException | None = None

    def map(self, path: str, kind: int) -> bytes:
        import os
        if self.error is not None:
            return b""
        self.access.forbid_access = True
        try:
            result = self.callback(path, kind)
            self.storage = os.fsencode(result) if result is not None else b""
            if b"\0" in self.storage:
                raise ValueError("Staging path contains a null byte")
            return self.storage
        except BaseException as error:
            self.error = error
            return b""
        finally:
            self.access.forbid_access = False
