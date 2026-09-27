"""Small configuration-driven provider factory and truthful capabilities."""

from __future__ import annotations

import os
from typing import Mapping, Optional, Any

from ..errors import ProviderSelectionError
from .base import LlmProvider
from .dashscope import DashScopeConfig, DashScopeProvider, DashScopeTransport
from .fake import FakeLlmProvider


def create_llm_provider(
    provider: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
    transport: Optional[DashScopeTransport] = None,
    sleeper: Any = None,
) -> LlmProvider:
    values = os.environ if environ is None else environ
    selected = (provider or values.get("WEAKNET_LLM_PROVIDER", "fake")).strip().lower()
    if selected == "fake":
        return FakeLlmProvider()
    if selected == "dashscope":
        return DashScopeProvider(
            DashScopeConfig.from_env(values), transport=transport, sleeper=sleeper
        )
    raise ProviderSelectionError(f"unsupported LLM provider: {selected}")


def provider_capabilities(environ: Optional[Mapping[str, str]] = None) -> dict[str, Any]:
    values = os.environ if environ is None else environ
    configured = bool(values.get("DASHSCOPE_API_KEY"))
    return {
        "providers": {
            "fake": {"available": True, "configured": True, "simulated": True},
            "dashscope": {
                "configured": configured,
                "available": False,
                "simulated": False,
                "reachable": False,
                "model": values.get("WEAKNET_LLM_MODEL", "qwen-plus"),
            },
        },
        "rag_available": False,
    }

