"""Diagnosis source abstraction kept independent of HTTP and providers."""

from __future__ import annotations

from typing import Protocol

from ..schemas.diagnosis import DiagnosisSnapshot


class DiagnosisSource(Protocol):
    async def current(self) -> DiagnosisSnapshot:
        """Return the current canonical deterministic diagnosis."""
