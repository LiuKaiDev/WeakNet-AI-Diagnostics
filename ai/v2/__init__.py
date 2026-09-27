"""AI V2.1 structured diagnosis explanation framework.

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
]
