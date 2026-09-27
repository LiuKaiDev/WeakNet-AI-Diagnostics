"""Minimal provider protocol for a future real structured-output backend."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Protocol, TYPE_CHECKING

if TYPE_CHECKING:
    from ..schemas.diagnosis import DiagnosisSnapshot


@dataclass(frozen=True)
class LlmRequest:
    system_prompt: str
    user_prompt: str
    response_schema_version: str
    request_id: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)
    snapshot: "DiagnosisSnapshot | None" = None


@dataclass
class LlmProviderResult:
    provider: str
    model: str
    structured_payload: Any
    latency_ms: int | None = None
    finish_status: str = "completed"
    error: str | None = None


class LlmProvider(Protocol):
    async def generate(self, request: LlmRequest) -> LlmProviderResult:
        """Return structured provider output; implementations must not mutate input."""
