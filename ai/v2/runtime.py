"""Read-only localhost HTTP adapter for structured explanation and RAG advice.

Run from the repository root with ``python -m ai.v2.runtime``.  The core
explainer and diagnosis source remain independent of this HTTP adapter.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from typing import Any, Mapping
from urllib.parse import urlsplit

from .errors import AiLayerError, ProviderUnavailable
from .providers.factory import create_llm_provider, provider_capabilities
from .schemas.diagnosis import DiagnosisSnapshot, DiagnosisValidationError, InputTooLarge
from .services.explainer import EvidenceExplainerService
from .sources.base import DiagnosisSource
from .sources.dbus import DbusDiagnosisSource
from .rag.capabilities import capabilities as rag_capabilities
from .services.advisor import RagAdvisorService
from .rag.knowledge import CorpusManifest, default_manifest_path

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765
MAX_REQUEST_BYTES = 256 * 1024


class _UnavailableLlmProvider:
    """Preserves retrieval when explicit provider configuration is unavailable."""
    async def generate(self, request: Any) -> Any:
        raise ProviderUnavailable("configured AI provider is unavailable")


def _public_error(error: BaseException) -> tuple[str, str]:
    category = getattr(error, "category", "InternalError")
    messages = {
        "DiagnosisSourceUnavailable": "weaknetd diagnosis is unavailable",
        "DiagnosisSourceTimeout": "weaknetd diagnosis timed out",
        "MalformedDiagnosisSourcePayload": "weaknetd diagnosis payload is invalid",
        "InvalidDiagnosisInput": "diagnosis input is invalid",
        "InputTooLarge": "diagnosis input exceeds service limits",
        "ProviderUnavailable": "AI provider is unavailable",
        "ProviderAuthenticationError": "provider authentication failed",
        "ProviderRateLimited": "provider rate limit reached",
        "ProviderTimeout": "provider timeout",
        "ProviderRequestError": "provider rejected the request",
        "ProviderServerError": "provider server error",
        "InvalidProviderOutput": "invalid model output",
        "GroundingViolation": "grounding validation failed",
        "InvalidProviderConfiguration": "AI provider configuration is invalid",
    }
    return str(category), messages.get(str(category), "AI explanation failed")


@dataclass
class AiExplanationApplication:
    """Transport-neutral request application."""

    explainer: EvidenceExplainerService | None
    diagnosis_source: DiagnosisSource
    selected_provider: str
    provider_error: AiLayerError | None = None
    environ: Mapping[str, str] | None = None
    advisor: RagAdvisorService | None = None

    @classmethod
    def from_env(
        cls, environ: Mapping[str, str] | None = None,
        diagnosis_source: DiagnosisSource | None = None,
    ) -> "AiExplanationApplication":
        values = os.environ if environ is None else environ
        selected = values.get("WEAKNET_LLM_PROVIDER", "fake").strip().lower()
        provider = None
        try:
            provider = create_llm_provider(selected, environ=values)
            explainer = EvidenceExplainerService(provider)
            provider_error = None
        except AiLayerError as exc:
            # The service remains live so health/capabilities can explain why
            # the explicitly selected provider cannot serve requests.
            explainer = None
            provider_error = exc
        return cls(
            explainer=explainer,
            diagnosis_source=diagnosis_source or DbusDiagnosisSource(),
            selected_provider=selected,
            provider_error=provider_error,
            environ=values,
            advisor=RagAdvisorService(provider or _UnavailableLlmProvider()),
        )

    def capabilities(self) -> dict[str, Any]:
        result = provider_capabilities(self.environ)
        result["selected_provider"] = self.selected_provider
        result["explanations_available"] = self.explainer is not None
        result["rag_advice_available"] = self.advisor is not None and self.provider_error is None
        result["rag_retrieval_mode"] = "lexical"
        result["diagnosis_source"] = "dbus-v2"
        if self.provider_error is not None:
            category, message = _public_error(self.provider_error)
            result["provider_error"] = {"category": category, "message": message}
        return result

    def rag_capabilities(self) -> dict[str, Any]:
        """Report optional retrieval availability without loading models."""
        try:
            path = default_manifest_path()
            CorpusManifest.load(path)
            corpus_available = True
        except Exception:
            corpus_available = False
        return rag_capabilities(corpus_available=corpus_available).to_dict()

    async def explain(self, snapshot: DiagnosisSnapshot, request_id: str = "") -> dict[str, Any]:
        if self.explainer is None:
            raise self.provider_error or ProviderUnavailable("provider unavailable")
        return (await self.explainer.explain(snapshot, request_id)).to_dict()

    async def explain_current(self, request_id: str = "") -> dict[str, Any]:
        snapshot = await self.diagnosis_source.current()
        return await self.explain(snapshot, request_id)

    async def advise(self, snapshot: DiagnosisSnapshot, request_id: str = "") -> dict[str, Any]:
        if self.advisor is None:
            raise ProviderUnavailable("RAG advisor is unavailable")
        return (await self.advisor.advise(snapshot, request_id=request_id)).to_dict()

    async def advise_current(self, request_id: str = "") -> dict[str, Any]:
        snapshot = await self.diagnosis_source.current()
        return await self.advise(snapshot, request_id)

    def handle(self, method: str, path: str, body: bytes = b"") -> tuple[int, dict[str, Any]]:
        try:
            if method == "GET" and path == "/health/live":
                return 200, {"status": "live"}
            if method == "GET" and path == "/v2/capabilities":
                return 200, self.capabilities()
            if method == "GET" and path == "/v2/rag/capabilities":
                return 200, self.rag_capabilities()
            if method == "POST" and path == "/v2/explanations":
                payload = json.loads(body.decode("utf-8"))
                snapshot = DiagnosisSnapshot.from_dict(payload)
                request_id = str(payload.get("request_id", "")) if isinstance(payload, dict) else ""
                return 200, asyncio.run(self.explain(snapshot, request_id))
            if method == "POST" and path == "/v2/explanations/current":
                request_id = ""
                if body and body.strip() not in (b"", b"{}"):
                    request = json.loads(body.decode("utf-8"))
                    if not isinstance(request, dict):
                        raise DiagnosisValidationError("request body must be an object")
                    request_id = str(request.get("request_id", ""))
                return 200, asyncio.run(self.explain_current(request_id))
            if method == "POST" and path == "/v2/advice":
                payload = json.loads(body.decode("utf-8"))
                snapshot = DiagnosisSnapshot.from_dict(payload)
                request_id = str(payload.get("request_id", "")) if isinstance(payload, dict) else ""
                return 200, asyncio.run(self.advise(snapshot, request_id))
            if method == "POST" and path == "/v2/advice/current":
                request_id = ""
                if body and body.strip() not in (b"", b"{}"):
                    request = json.loads(body.decode("utf-8"))
                    if not isinstance(request, dict):
                        raise DiagnosisValidationError("request body must be an object")
                    request_id = str(request.get("request_id", ""))
                return 200, asyncio.run(self.advise_current(request_id))
            return 404, {"error": {"category": "NotFound", "message": "endpoint not found"}}
        except InputTooLarge as exc:
            category, message = _public_error(exc)
            return 413, {"error": {"category": category, "message": message}}
        except (json.JSONDecodeError, UnicodeDecodeError, DiagnosisValidationError) as exc:
            category, message = _public_error(exc)
            return 400, {"error": {"category": category, "message": message}}
        except AiLayerError as exc:
            category, message = _public_error(exc)
            return 503, {"error": {"category": category, "message": message}}
        except Exception:
            return 500, {"error": {"category": "InternalError", "message": "AI service internal error"}}


class _Handler(BaseHTTPRequestHandler):
    server_version = "WeakNetAI/2"

    @property
    def app(self) -> AiExplanationApplication:
        return self.server.application  # type: ignore[attr-defined,no-any-return]

    def do_GET(self) -> None:  # noqa: N802 - stdlib handler API
        self._dispatch(b"")

    def do_POST(self) -> None:  # noqa: N802 - stdlib handler API
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._write(400, {"error": {"category": "InvalidRequest", "message": "invalid content length"}})
            return
        if length < 0 or length > MAX_REQUEST_BYTES:
            self._write(413, {"error": {"category": "InputTooLarge", "message": "request too large"}})
            return
        self._dispatch(self.rfile.read(length))

    def _dispatch(self, body: bytes) -> None:
        status, payload = self.app.handle(self.command, urlsplit(self.path).path, body)
        self._write(status, payload)

    def _write(self, status: int, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, format: str, *args: Any) -> None:
        # Prompt/request bodies and credentials are never logged.
        return


def create_http_server(
    application: AiExplanationApplication,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
) -> ThreadingHTTPServer:
    if host not in ("127.0.0.1", "::1", "localhost"):
        raise ValueError("AI service must bind to a loopback address")
    if port < 1 or port > 65535:
        raise ValueError("AI service port is invalid")
    server = ThreadingHTTPServer((host, port), _Handler)
    server.application = application  # type: ignore[attr-defined]
    return server


def main() -> int:
    host = os.environ.get("WEAKNET_AI_HOST", DEFAULT_HOST)
    try:
        port = int(os.environ.get("WEAKNET_AI_PORT", str(DEFAULT_PORT)))
        server = create_http_server(AiExplanationApplication.from_env(), host, port)
    except (ValueError, OSError) as exc:
        print(f"weaknet-ai: startup failed: {exc}")
        return 2
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
