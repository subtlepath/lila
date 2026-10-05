"""YAML loading with line numbers, duplicate-key detection and YAML 1.2 booleans.

YAML 1.1 reads `no`, `yes`, `on` and `off` as booleans, which would turn the
Spanish word `no` in a list of lemmas into False. Only true/false are booleans
here.
"""

from __future__ import annotations

import re

import yaml


class LineDict(dict):
    line = 0


class LineList(list):
    line = 0


class YamlError(ValueError):
    def __init__(self, path: str, line: int, message: str):
        super().__init__(f"{path}:{line}: {message}")
        self.path = path
        self.line = line


# libyaml's parser when available (several times faster); same constructors either way.
class _Loader(getattr(yaml, "CSafeLoader", yaml.SafeLoader)):
    pass


_BOOL = "tag:yaml.org,2002:bool"
_Loader.yaml_implicit_resolvers = {
    first: [(tag, rx) for tag, rx in resolvers if tag != _BOOL]
    for first, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
}
_Loader.add_implicit_resolver(_BOOL, re.compile(r"^(?:true|True|TRUE|false|False|FALSE)$"), list("tTfF"))


def _mapping(loader: _Loader, node: yaml.MappingNode) -> LineDict:
    loader.flatten_mapping(node)
    out = LineDict()
    out.line = node.start_mark.line + 1
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        if key in out:
            raise yaml.constructor.ConstructorError(
                None, None, f"duplicate key {key!r}", key_node.start_mark)
        out[key] = loader.construct_object(value_node, deep=True)
    return out


def _sequence(loader: _Loader, node: yaml.SequenceNode) -> LineList:
    out = LineList(loader.construct_object(child, deep=True) for child in node.value)
    out.line = node.start_mark.line + 1
    return out


_Loader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _mapping)
_Loader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_SEQUENCE_TAG, _sequence)


def load(path: str):
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    try:
        return yaml.load(text, Loader=_Loader)  # noqa: S506 (safe loader subclass)
    except yaml.MarkedYAMLError as exc:
        mark = exc.problem_mark or exc.context_mark
        line = mark.line + 1 if mark else 0
        raise YamlError(path, line, f"{exc.problem or exc}") from None
    except yaml.YAMLError as exc:
        raise YamlError(path, 0, str(exc)) from None


def loads(text: str):
    """Parse a YAML snippet (a flow mapping in a TSV comment, for example)."""
    return yaml.load(text, Loader=_Loader)  # noqa: S506 (safe loader subclass)


def plain(value):
    """Deep copy into plain dicts and lists (for hashing and JSON)."""
    if isinstance(value, dict):
        return {str(k): plain(v) for k, v in value.items()}
    if isinstance(value, list):
        return [plain(v) for v in value]
    if hasattr(value, "isoformat"):
        return value.isoformat()
    return value
