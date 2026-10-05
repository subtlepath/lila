"""Collected compiler diagnostics. The compiler reports every problem it can find
in one run instead of stopping at the first."""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Where:
    path: str
    line: int = 0
    what: str = ""  # e.g. "sentence 3"

    def __str__(self) -> str:
        rel = os.path.relpath(self.path) if os.path.isabs(self.path) else self.path
        loc = f"{rel}:{self.line}" if self.line else rel
        return f"{loc}: {self.what}" if self.what else loc

    def at(self, what: str, line: int | None = None) -> "Where":
        return Where(self.path, self.line if line is None else line, what)


@dataclass
class Message:
    level: str  # "error" | "warning"
    code: str
    where: Where | None
    text: str

    def __str__(self) -> str:
        prefix = f"{self.where}: " if self.where else ""
        return f"{prefix}{self.level} [{self.code}]: {self.text}"


class Diagnostics:
    def __init__(self) -> None:
        self.messages: list[Message] = []

    def error(self, where: Where | None, code: str, text: str) -> None:
        self.messages.append(Message("error", code, where, text))

    def warn(self, where: Where | None, code: str, text: str) -> None:
        self.messages.append(Message("warning", code, where, text))

    def note(self, where: Where | None, code: str, text: str) -> None:
        """Information for the content lead; never fails a build."""
        self.messages.append(Message("note", code, where, text))

    @property
    def errors(self) -> list[Message]:
        return [m for m in self.messages if m.level == "error"]

    @property
    def warnings(self) -> list[Message]:
        return [m for m in self.messages if m.level == "warning"]

    def codes(self) -> set[str]:
        return {m.code for m in self.errors}
