"""Structured diagnosis explanation and grounded RAG framework.

This package makes no external network or model calls by itself.  The default
provider is the deterministic fake provider used by tests and local demos.
"""

from .schemas.diagnosis import (
    DIAGNOSIS_SCHEMA_VERSION,
    DiagnosisSnapshot,
    DiagnosisValidationError,
    Evidence,
    EvidenceRole,
    InputTooLarge,
    Incident,
    RootCauseHypothesis,
)
from .schemas.explanation import (
    EXPLANATION_SCHEMA_VERSION,
    ExplanationReport,
    ProviderExplanationPayload,
)
from .providers.dashscope import DashScopeConfig, DashScopeProvider
from .providers.factory import create_llm_provider, provider_capabilities

__all__ = [
    "DIAGNOSIS_SCHEMA_VERSION",
    "DiagnosisSnapshot",
    "DiagnosisValidationError",
    "Evidence",
    "EvidenceRole",
    "InputTooLarge",
    "Incident",
    "RootCauseHypothesis",
    "EXPLANATION_SCHEMA_VERSION",
    "ExplanationReport",
    "ProviderExplanationPayload",
    "DashScopeConfig",
    "DashScopeProvider",
    "create_llm_provider",
    "provider_capabilities",
]
