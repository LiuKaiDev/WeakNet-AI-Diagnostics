"""Explicit, allowlisted knowledge corpus loading."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from typing import Any

from .schemas import KnowledgeDocument, CORPUS_SCHEMA_VERSION


class KnowledgeLoadError(RuntimeError):
    pass


@dataclass(frozen=True)
class CorpusManifest:
    corpus_version: str
    documents: tuple[dict[str, Any], ...]

    @classmethod
    def load(cls, path: str | Path) -> "CorpusManifest":
        manifest_path = Path(path)
        try:
            payload = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise KnowledgeLoadError("invalid corpus manifest") from exc
        if not isinstance(payload, dict) or not payload.get("corpus_version") or not isinstance(payload.get("documents"), list):
            raise KnowledgeLoadError("manifest requires corpus_version and documents")
        return cls(str(payload["corpus_version"]), tuple(payload["documents"]))

    def to_dict(self) -> dict[str, Any]:
        return {"corpus_version": self.corpus_version, "documents": list(self.documents)}


def load_documents(manifest: CorpusManifest | str | Path, *, root: str | Path | None = None) -> list[KnowledgeDocument]:
    loaded = manifest if isinstance(manifest, CorpusManifest) else CorpusManifest.load(manifest)
    manifest_path = Path(manifest) if not isinstance(manifest, CorpusManifest) else None
    base = Path(root) if root is not None else (manifest_path.parent if manifest_path else Path.cwd())
    result: list[KnowledgeDocument] = []
    for spec in sorted(loaded.documents, key=lambda item: str(item.get("id", ""))):
        if not isinstance(spec, dict) or not spec.get("id") or not spec.get("path"):
            raise KnowledgeLoadError("manifest document requires id and path")
        relative = Path(str(spec["path"]))
        if relative.is_absolute() or ".." in relative.parts:
            raise KnowledgeLoadError("manifest paths must stay below manifest root")
        path = (base / relative).resolve()
        try:
            path.relative_to(base.resolve())
            content = path.read_text(encoding="utf-8")
        except (OSError, ValueError) as exc:
            raise KnowledgeLoadError(f"allowlisted knowledge source unavailable: {relative}") from exc
        result.append(KnowledgeDocument(
            document_id=str(spec["id"]), title=str(spec.get("title", spec["id"])),
            source=str(spec.get("source", relative.as_posix())), source_type=str(spec.get("source_type", "repository-document")),
            version=str(spec.get("version", loaded.corpus_version)), content=content,
            updated_at=spec.get("updated_at"), tags=tuple(sorted(map(str, spec.get("tags", [])))),
            applicable_root_causes=tuple(sorted(map(str, spec.get("applicable_root_causes", [])))),
            platform=spec.get("platform"), required_capabilities=tuple(sorted(map(str, spec.get("required_capabilities", [])))),
        ))
    return result


def corpus_fingerprint(documents: list[KnowledgeDocument], corpus_version: str) -> str:
    encoded = json.dumps({"version": corpus_version, "documents": [item.to_dict() for item in documents]}, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(encoded.encode()).hexdigest()


def default_manifest_path() -> Path:
    return Path(__file__).resolve().parents[2] / "knowledge" / "manifest.json"
