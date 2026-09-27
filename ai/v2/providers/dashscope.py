"""Small, replaceable DashScope/Qwen structured-output provider.

The default transport uses Python's standard-library HTTPS client.  Tests can
inject ``DashScopeTransport`` and never perform network I/O.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field, replace
import json
import os
import time
from typing import Any, Callable, Mapping, Optional, Protocol
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from ..errors import (
    InvalidProviderOutput,
    ProviderAuthenticationError,
    ProviderRateLimited,
    ProviderRequestError,
    ProviderServerError,
    ProviderTimeout,
    ProviderUnavailable,
)
from ..schemas.explanation import EXPLANATION_SCHEMA_VERSION
from ..schemas.diagnosis import InputTooLarge
from .base import LlmProviderResult, LlmRequest


DEFAULT_DASHSCOPE_ENDPOINT = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
DEFAULT_MODEL = "qwen-plus"
DEFAULT_CONNECT_TIMEOUT_SECONDS = 5.0
DEFAULT_READ_TIMEOUT_SECONDS = 25.0
DEFAULT_OVERALL_TIMEOUT_SECONDS = 30.0
DEFAULT_MAX_RESPONSE_BYTES = 64 * 1024
DEFAULT_MAX_PROMPT_BYTES = 256 * 1024
DEFAULT_MAX_ATTEMPTS = 2
DEFAULT_TEMPERATURE = 0.2


@dataclass(frozen=True)
class DashScopeConfig:
    """Configuration with secrets excluded from repr and provider metadata."""

    api_key: str = field(default="", repr=False)
    model: str = DEFAULT_MODEL
    endpoint: str = DEFAULT_DASHSCOPE_ENDPOINT
    connect_timeout_seconds: float = DEFAULT_CONNECT_TIMEOUT_SECONDS
    read_timeout_seconds: float = DEFAULT_READ_TIMEOUT_SECONDS
    overall_timeout_seconds: float = DEFAULT_OVERALL_TIMEOUT_SECONDS
    max_response_bytes: int = DEFAULT_MAX_RESPONSE_BYTES
    max_prompt_bytes: int = DEFAULT_MAX_PROMPT_BYTES
    max_attempts: int = DEFAULT_MAX_ATTEMPTS
    temperature: float = DEFAULT_TEMPERATURE
    retry_backoff_seconds: float = 0.25
    retry_jitter_seconds: float = 0.0

    @classmethod
    def from_env(cls, environ: Optional[Mapping[str, str]] = None) -> "DashScopeConfig":
        values = os.environ if environ is None else environ
        api_key = values.get("DASHSCOPE_API_KEY", "")
        model = values.get("WEAKNET_LLM_MODEL", DEFAULT_MODEL)
        return cls(api_key=api_key, model=model)

    def validate(self) -> None:
        if not self.api_key:
            raise ProviderUnavailable("DASHSCOPE_API_KEY is not configured")
        if not self.model or len(self.model) > 256:
            raise ProviderRequestError("DashScope model configuration is invalid")
        if not self.endpoint.startswith("https://"):
            raise ProviderRequestError("DashScope endpoint must use HTTPS")
        if self.connect_timeout_seconds <= 0 or self.read_timeout_seconds <= 0:
            raise ProviderRequestError("DashScope timeouts must be positive")
        if self.overall_timeout_seconds <= 0 or self.overall_timeout_seconds > 35:
            raise ProviderRequestError("DashScope overall timeout must be between 0 and 35 seconds")
        if self.max_response_bytes <= 0 or self.max_prompt_bytes <= 0:
            raise ProviderRequestError("DashScope size limits must be positive")
        if self.max_attempts < 1 or self.max_attempts > 3:
            raise ProviderRequestError("DashScope max_attempts must be between 1 and 3")
        if not 0.0 <= self.temperature <= 1.0:
            raise ProviderRequestError("DashScope temperature must be between 0 and 1")

    def public_dict(self) -> dict[str, Any]:
        return {
            "provider": "dashscope",
            "model": self.model,
            "endpoint": self.endpoint,
            "connect_timeout_seconds": self.connect_timeout_seconds,
            "read_timeout_seconds": self.read_timeout_seconds,
            "overall_timeout_seconds": self.overall_timeout_seconds,
            "max_response_bytes": self.max_response_bytes,
            "max_prompt_bytes": self.max_prompt_bytes,
            "max_attempts": self.max_attempts,
            "temperature": self.temperature,
        }


@dataclass(frozen=True)
class DashScopeResponse:
    status_code: int
    body: bytes
    provider_request_id: Optional[str] = None
    headers: Mapping[str, str] = field(default_factory=dict)


class DashScopeTransport(Protocol):
    def post_json(
        self, endpoint: str, headers: Mapping[str, str], payload: Mapping[str, Any],
        timeout_seconds: float, max_response_bytes: int,
    ) -> DashScopeResponse:
        """Perform one bounded request. Implementations must not log headers."""


class UrllibDashScopeTransport:
    def post_json(
        self, endpoint: str, headers: Mapping[str, str], payload: Mapping[str, Any],
        timeout_seconds: float, max_response_bytes: int,
    ) -> DashScopeResponse:
        body = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
        request = Request(endpoint, data=body, headers=dict(headers), method="POST")
        try:
            with urlopen(request, timeout=timeout_seconds) as response:  # nosec B310: endpoint is validated HTTPS
                data = response.read(max_response_bytes + 1)
                response_headers = {str(key): str(value) for key, value in response.headers.items()}
                return DashScopeResponse(
                    status_code=int(response.status), body=data,
                    provider_request_id=response_headers.get("x-request-id"),
                    headers=response_headers,
                )
        except HTTPError as error:
            data = error.read(max_response_bytes + 1)
            response_headers = {str(key): str(value) for key, value in error.headers.items()}
            return DashScopeResponse(
                status_code=int(error.code), body=data,
                provider_request_id=response_headers.get("x-request-id"),
                headers=response_headers,
            )
        except TimeoutError as error:
            raise TimeoutError("DashScope request timed out") from error
        except URLError as error:
            raise ConnectionError("DashScope transport unavailable") from error


class DashScopeProvider:
    provider_name = "dashscope"
    simulated = False

    def __init__(
        self, config: Optional[DashScopeConfig] = None,
        transport: Optional[DashScopeTransport] = None,
        sleeper: Optional[Callable[[float], None]] = None,
        clock: Callable[[], float] = time.monotonic,
        api_key: Optional[str] = None,
        model: Optional[str] = None,
    ) -> None:
        self.config = config or DashScopeConfig.from_env()
        if api_key is not None or model is not None:
            self.config = replace(
                self.config,
                api_key=self.config.api_key if api_key is None else api_key,
                model=self.config.model if model is None else model,
            )
        self.transport = transport or UrllibDashScopeTransport()
        self.sleeper = sleeper or time.sleep
        self.clock = clock
        self.config.validate()

    @classmethod
    def from_env(
        cls, environ: Optional[Mapping[str, str]] = None,
        transport: Optional[DashScopeTransport] = None,
        sleeper: Optional[Callable[[float], None]] = None,
    ) -> "DashScopeProvider":
        return cls(DashScopeConfig.from_env(environ), transport=transport, sleeper=sleeper)

    def __repr__(self) -> str:
        return f"DashScopeProvider(config={self.config.public_dict()!r})"

    async def generate(self, request: LlmRequest) -> LlmProviderResult:
        prompt_bytes = len(request.user_prompt.encode("utf-8")) + len(request.system_prompt.encode("utf-8"))
        if prompt_bytes > self.config.max_prompt_bytes:
            raise InputTooLarge("DashScope prompt exceeds configured size limit")
        payload = self._request_payload(request)
        headers = {
            "Authorization": f"Bearer {self.config.api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        }
        if request.request_id:
            headers["X-Request-ID"] = request.request_id
        started = self.clock()
        deadline = started + self.config.overall_timeout_seconds
        last_error: Optional[Exception] = None
        for attempt in range(1, self.config.max_attempts + 1):
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise ProviderTimeout("DashScope overall request timeout exceeded")
            timeout = min(self.config.read_timeout_seconds, remaining)
            try:
                response = await asyncio.wait_for(
                    asyncio.to_thread(
                        self.transport.post_json, self.config.endpoint, headers, payload,
                        timeout, self.config.max_response_bytes,
                    ),
                    timeout=remaining,
                )
                self._raise_for_status(response.status_code)
                parsed, finish_status = self._parse_response(response.body)
                return LlmProviderResult(
                    provider=self.provider_name,
                    model=self.config.model,
                    structured_payload=parsed,
                    latency_ms=max(0, int((self.clock() - started) * 1000)),
                    finish_status=finish_status,
                    request_id=request.request_id,
                    provider_request_id=response.provider_request_id,
                )
            except (ProviderAuthenticationError, ProviderRequestError,
                    InvalidProviderOutput, ProviderTimeout):
                raise
            except (ProviderRateLimited, ProviderServerError, ProviderUnavailable) as error:
                last_error = error
                if attempt >= self.config.max_attempts:
                    raise
                await self._backoff(attempt, deadline)
            except TimeoutError as error:
                raise ProviderTimeout("DashScope request timed out") from error
            except (ConnectionError, OSError) as error:
                last_error = ProviderUnavailable("DashScope transport unavailable")
                if attempt >= self.config.max_attempts:
                    raise last_error from error
                await self._backoff(attempt, deadline)
            except asyncio.TimeoutError as error:
                raise ProviderTimeout("DashScope request timed out") from error
        raise last_error or ProviderUnavailable("DashScope request failed")

    def _request_payload(self, request: LlmRequest) -> dict[str, Any]:
        return {
            "model": self.config.model,
            "temperature": self.config.temperature,
            "stream": False,
            "response_format": {"type": "json_object"},
            "messages": [
                {"role": "system", "content": request.system_prompt},
                {"role": "user", "content": request.user_prompt},
            ],
        }

    @staticmethod
    def _raise_for_status(status_code: int) -> None:
        if 200 <= status_code < 300:
            return
        if status_code in (401, 403):
            raise ProviderAuthenticationError("DashScope authentication failed")
        if status_code == 429:
            raise ProviderRateLimited("DashScope rate limit reached")
        if 400 <= status_code < 500:
            raise ProviderRequestError(f"DashScope request rejected (HTTP {status_code})")
        if status_code >= 500:
            raise ProviderServerError(f"DashScope server error (HTTP {status_code})")
        raise ProviderRequestError(f"DashScope unexpected HTTP status ({status_code})")

    def _parse_response(self, body: bytes) -> tuple[dict[str, Any], str]:
        if len(body) > self.config.max_response_bytes:
            raise InvalidProviderOutput("DashScope response exceeds configured size limit")
        try:
            envelope = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise InvalidProviderOutput("DashScope response is not valid JSON") from error
        if not isinstance(envelope, dict):
            raise InvalidProviderOutput("DashScope response must be a JSON object")
        choices = envelope.get("choices")
        if not isinstance(choices, list) or len(choices) != 1 or not isinstance(choices[0], dict):
            raise InvalidProviderOutput("DashScope response has no single structured choice")
        choice = choices[0]
        message = choice.get("message")
        if not isinstance(message, dict) or not isinstance(message.get("content"), str):
            raise InvalidProviderOutput("DashScope response content is missing")
        content = message["content"].strip()
        if not content.startswith("{") or not content.endswith("}"):
            raise InvalidProviderOutput("DashScope content must be exactly one JSON object")
        try:
            payload = json.loads(content)
        except json.JSONDecodeError as error:
            raise InvalidProviderOutput("DashScope content is not valid JSON") from error
        if not isinstance(payload, dict):
            raise InvalidProviderOutput("DashScope structured output must be an object")
        finish_status = str(choice.get("finish_reason", "completed"))
        if finish_status not in ("stop", "completed", "success"):
            raise InvalidProviderOutput("DashScope response did not finish with complete JSON")
        return payload, finish_status

    async def _backoff(self, attempt: int, deadline: float) -> None:
        delay = self.config.retry_backoff_seconds * (2 ** (attempt - 1))
        if self.config.retry_jitter_seconds:
            # A deterministic provider has no need for random jitter in tests;
            # callers may configure a fixed bound at the transport boundary.
            delay += self.config.retry_jitter_seconds
        remaining = deadline - self.clock()
        if remaining <= 0:
            raise ProviderTimeout("DashScope overall request timeout exceeded")
        await asyncio.to_thread(self.sleeper, min(delay, remaining))
