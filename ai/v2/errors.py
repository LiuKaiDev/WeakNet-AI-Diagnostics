"""Shared AI-layer error categories.

Messages from this module are deliberately safe for logs: providers must not
include authorization headers, API keys, or raw response bodies.
"""

from __future__ import annotations


class AiLayerError(RuntimeError):
    category = "InternalError"


class InvalidDiagnosisInput(AiLayerError):
    category = "InvalidDiagnosisInput"


class ProviderUnavailable(AiLayerError):
    category = "ProviderUnavailable"


class ProviderAuthenticationError(AiLayerError):
    category = "ProviderAuthenticationError"


class ProviderRateLimited(AiLayerError):
    category = "ProviderRateLimited"


class ProviderTimeout(AiLayerError):
    category = "ProviderTimeout"


class ProviderRequestError(AiLayerError):
    category = "ProviderRequestError"


class ProviderServerError(AiLayerError):
    category = "ProviderServerError"


class InvalidProviderOutput(AiLayerError):
    category = "InvalidProviderOutput"


class GroundingValidationError(AiLayerError):
    category = "GroundingViolation"


class ProviderConfigurationError(AiLayerError):
    category = "ProviderUnavailable"


class ProviderSelectionError(AiLayerError):
    category = "InvalidProviderConfiguration"

