"""Stable Markdown-aware knowledge chunking."""
from __future__ import annotations

import re
from .schemas import KnowledgeDocument, KnowledgeChunk


def _sections(content: str) -> list[tuple[str, str]]:
    current = "Introduction"
    lines: list[str] = []
    result: list[tuple[str, str]] = []
    for line in content.splitlines():
        match = re.match(r"^#{1,6}\s+(.+?)\s*$", line)
        if match:
            if "\n".join(lines).strip():
                result.append((current, "\n".join(lines).strip()))
            current, lines = match.group(1), []
        else:
            lines.append(line)
    if "\n".join(lines).strip():
        result.append((current, "\n".join(lines).strip()))
    return result


def chunk_document(document: KnowledgeDocument, *, max_chars: int = 1800, overlap_chars: int = 180) -> list[KnowledgeChunk]:
    if max_chars <= 0 or overlap_chars < 0 or overlap_chars >= max_chars:
        raise ValueError("invalid chunk settings")
    chunks: list[KnowledgeChunk] = []
    for section, body in _sections(document.content):
        paragraphs = [part.strip() for part in re.split(r"\n\s*\n", body) if part.strip()]
        pieces: list[str] = []
        for paragraph in paragraphs:
            if len(paragraph) > max_chars and any(len(word) > max_chars for word in paragraph.split()):
                # A pathological unbroken token has no semantic split point.
                # Bound it deterministically as a final safety measure.
                remaining = paragraph
                while remaining:
                    pieces.append(remaining[:max_chars])
                    remaining = remaining[max_chars:]
                continue
            if len(paragraph) <= max_chars:
                pieces.append(paragraph)
                continue
            # Long paragraphs are split on whitespace only as a last resort.
            words, current = paragraph.split(), ""
            for word in words:
                candidate = f"{current} {word}".strip()
                if current and len(candidate) > max_chars:
                    pieces.append(current)
                    tail = current[-overlap_chars:] if overlap_chars else ""
                    available = max_chars - len(word) - (1 if tail else 0)
                    current = f"{tail[-max(available, 0):]} {word}".strip() if available > 0 else word
                else:
                    current = candidate
            if current:
                pieces.append(current)
        for piece in pieces:
            if piece:
                chunks.append(KnowledgeChunk.create(document, section, piece, len(chunks)))
    return chunks


def chunk_documents(documents: list[KnowledgeDocument], **kwargs: object) -> list[KnowledgeChunk]:
    result: list[KnowledgeChunk] = []
    for document in sorted(documents, key=lambda d: d.document_id):
        result.extend(chunk_document(document, **kwargs))
    return result
