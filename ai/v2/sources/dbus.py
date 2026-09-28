"""Real, bounded D-Bus diagnosis source.

``dbus-next`` is imported only when the real client is used.  Fake-provider
service startup and all unit tests therefore remain independent of it.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any, Mapping, Protocol

from ..adapters.dbus import DbusDiagnosisAdapter
from ..errors import (
    DiagnosisSourceError,
    DiagnosisSourceTimeout,
    MalformedDiagnosisSourcePayload,
)
from ..schemas.diagnosis import DiagnosisSnapshot, DiagnosisValidationError

DBUS_SERVICE = "com.example.WeakNet"
DBUS_PATH = "/com/example/WeakNet/V2"
DBUS_INTERFACE = "com.example.WeakNet.Diagnostics2"
DBUS_METHOD = "GetDiagnosis"
DEFAULT_DBUS_TIMEOUT_SECONDS = 3.0


class DbusDiagnosisClient(Protocol):
    async def get_diagnosis(self) -> Mapping[str, Any]:
        """Call GetDiagnosis and return recursively decoded values."""


def _unwrap(value: Any) -> Any:
    # Avoid importing dbus-next types here; Variant exposes ``value`` and the
    # remaining containers are ordinary Python values.
    if value.__class__.__name__ == "Variant" and hasattr(value, "value"):
        return _unwrap(value.value)
    if isinstance(value, Mapping):
        return {str(key): _unwrap(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_unwrap(item) for item in value]
    return value


class DbusNextDiagnosisClient:
    """Session-bus client matching the implemented diagnostic contract."""

    async def get_diagnosis(self) -> Mapping[str, Any]:
        try:
            from dbus_next import BusType, Message, MessageType  # type: ignore[import-not-found]
            from dbus_next.aio import MessageBus  # type: ignore[import-not-found]
        except ImportError as exc:
            raise DiagnosisSourceError(
                "Python D-Bus support is unavailable (install dbus-next)"
            ) from exc

        bus = await MessageBus(bus_type=BusType.SESSION).connect()
        try:
            reply = await bus.call(Message(
                destination=DBUS_SERVICE,
                path=DBUS_PATH,
                interface=DBUS_INTERFACE,
                member=DBUS_METHOD,
            ))
            if reply.message_type == MessageType.ERROR:
                raise DiagnosisSourceError("weaknetd GetDiagnosis is unavailable")
            if len(reply.body) != 1:
                raise MalformedDiagnosisSourcePayload(
                    "GetDiagnosis returned an unexpected body"
                )
            decoded = _unwrap(reply.body[0])
            if not isinstance(decoded, Mapping):
                raise MalformedDiagnosisSourcePayload(
                    "GetDiagnosis did not return a dictionary"
                )
            return decoded
        finally:
            bus.disconnect()


@dataclass
class DbusDiagnosisSource:
    client: DbusDiagnosisClient = field(default_factory=DbusNextDiagnosisClient)
    adapter: DbusDiagnosisAdapter = field(default_factory=DbusDiagnosisAdapter)
    timeout_seconds: float = DEFAULT_DBUS_TIMEOUT_SECONDS

    async def current(self) -> DiagnosisSnapshot:
        if self.timeout_seconds <= 0 or self.timeout_seconds > 30:
            raise DiagnosisSourceError("D-Bus timeout configuration is invalid")
        try:
            payload = await asyncio.wait_for(
                self.client.get_diagnosis(), timeout=self.timeout_seconds
            )
        except asyncio.TimeoutError as exc:
            raise DiagnosisSourceTimeout("weaknetd GetDiagnosis timed out") from exc
        except DiagnosisSourceError:
            raise
        except (ConnectionError, OSError) as exc:
            raise DiagnosisSourceError("weaknetd D-Bus service is unavailable") from exc
        except Exception as exc:
            # Do not leak dbus-next implementation details or bus error text
            # through the product API.
            raise DiagnosisSourceError("weaknetd D-Bus service is unavailable") from exc
        try:
            return self.adapter.from_dict(payload)
        except (DiagnosisValidationError, TypeError, ValueError) as exc:
            raise MalformedDiagnosisSourcePayload(
                "weaknetd returned a malformed GetDiagnosis payload"
            ) from exc
