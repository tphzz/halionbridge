#!/usr/bin/env python3
"""Merge an source preset CSV into generated VST preset metadata."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions"
TEMPLATE_NAME_COLUMN = "target_preset_name"
INPUT_NAME_COLUMN = "preset name"


class MergeError(Exception):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Merge source preset metadata into a generated VST preset metadata CSV."
    )
    parser.add_argument("--template-csv", required=True, type=Path)
    parser.add_argument("--input-csv", required=True, type=Path)
    parser.add_argument("--output-csv", required=True, type=Path)
    parser.add_argument(
        "--refresh-mapping",
        action="store_true",
        help="Regenerate the mapping sidecar instead of reusing it.",
    )
    return parser.parse_args()


def preflight_environment() -> None:
    missing = [
        name
        for name in ("OPENROUTER_API_KEY", "OPENROUTER_API_MODEL")
        if not os.environ.get(name)
    ]
    if missing:
        raise MergeError(
            "missing required environment variable(s): "
            + ", ".join(missing)
        )


def read_csv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                raise MergeError(f"{path} has no CSV header")
            rows = [{key: value or "" for key, value in row.items()} for row in reader]
            return list(reader.fieldnames), rows
    except OSError as exc:
        raise MergeError(f"failed to read {path}: {exc}") from exc


def write_csv(path: Path, fieldnames: list[str], rows: list[dict[str, str]]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
    except OSError as exc:
        raise MergeError(f"failed to write {path}: {exc}") from exc


def normalize_name(value: str) -> str:
    normalized = value.strip().lower()
    normalized = re.sub(r"['\u2019]", "", normalized)
    normalized = re.sub(r"[^a-z0-9]+", "_", normalized)
    normalized = re.sub(r"_+", "_", normalized)
    return normalized.strip("_")


def require_column(fieldnames: list[str], column: str, path: Path) -> None:
    if column not in fieldnames:
        raise MergeError(f"{path} is missing required column {column!r}")


def find_duplicate_values(rows: list[dict[str, str]], column: str) -> dict[str, list[int]]:
    seen: dict[str, list[int]] = {}
    for index, row in enumerate(rows):
        seen.setdefault(row[column], []).append(index)
    return {value: indexes for value, indexes in seen.items() if len(indexes) > 1}


def preflight(
    template_path: Path,
    template_fields: list[str],
    template_rows: list[dict[str, str]],
    input_path: Path,
    input_fields: list[str],
    input_rows: list[dict[str, str]],
) -> dict[str, list[int]]:
    require_column(template_fields, TEMPLATE_NAME_COLUMN, template_path)
    require_column(input_fields, INPUT_NAME_COLUMN, input_path)

    duplicate_template_names = find_duplicate_values(template_rows, TEMPLATE_NAME_COLUMN)
    if duplicate_template_names:
        details = ", ".join(
            f"{name!r} at rows {format_indexes(indexes)}"
            for name, indexes in sorted(duplicate_template_names.items())
        )
        raise MergeError(f"duplicate template {TEMPLATE_NAME_COLUMN!r} values: {details}")

    added_input_fields = [field for field in input_fields if field != INPUT_NAME_COLUMN]
    conflicts = [field for field in added_input_fields if field in template_fields]
    if conflicts:
        raise MergeError(
            "input columns conflict with template columns: " + ", ".join(repr(field) for field in conflicts)
        )

    return find_duplicate_values(input_rows, INPUT_NAME_COLUMN)


def format_indexes(indexes: list[int]) -> str:
    return ", ".join(str(index) for index in indexes)


def build_normalized_matches(
    template_rows: list[dict[str, str]], input_rows: list[dict[str, str]]
) -> tuple[dict[int, dict[str, Any]], list[str]]:
    input_by_normalized: dict[str, list[int]] = {}
    for input_index, row in enumerate(input_rows):
        input_by_normalized.setdefault(normalize_name(row[INPUT_NAME_COLUMN]), []).append(input_index)

    mappings: dict[int, dict[str, Any]] = {}
    notes: list[str] = []
    used_input_indexes: set[int] = set()
    for template_index, row in enumerate(template_rows):
        template_name = row[TEMPLATE_NAME_COLUMN]
        candidates = input_by_normalized.get(template_name, [])
        if len(candidates) == 1 and candidates[0] not in used_input_indexes:
            input_index = candidates[0]
            used_input_indexes.add(input_index)
            mappings[template_index] = make_mapping(
                template_index, template_name, input_index, input_rows[input_index][INPUT_NAME_COLUMN], "normalized"
            )
        elif len(candidates) > 1:
            notes.append(
                f"template row {template_index} {template_name!r} has ambiguous normalized input candidates "
                f"{format_indexes(candidates)}"
            )
    return mappings, notes


def make_mapping(
    template_index: int,
    template_name: str,
    input_index: int,
    input_name: str,
    source: str,
    confidence: Any = None,
    reason: str = "",
) -> dict[str, Any]:
    mapping: dict[str, Any] = {
        "template_index": template_index,
        "template_name": template_name,
        "input_index": input_index,
        "input_name": input_name,
        "source": source,
    }
    if confidence is not None:
        mapping["confidence"] = confidence
    if reason:
        mapping["reason"] = reason
    return mapping


def mapping_sidecar_path(output_csv: Path) -> Path:
    return output_csv.with_name(output_csv.name + ".mapping.json")


def load_sidecar(path: Path) -> list[dict[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
    except OSError as exc:
        raise MergeError(f"failed to read mapping sidecar {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise MergeError(f"invalid mapping sidecar JSON {path}: {exc}") from exc

    if isinstance(data, dict) and isinstance(data.get("mappings"), list):
        mappings = data["mappings"]
    elif isinstance(data, list):
        mappings = data
    else:
        raise MergeError(f"mapping sidecar {path} must contain a mappings list")

    if not all(isinstance(mapping, dict) for mapping in mappings):
        raise MergeError(f"mapping sidecar {path} contains non-object mappings")
    return mappings


def save_sidecar(path: Path, mappings: list[dict[str, Any]]) -> None:
    payload = {
        "version": 1,
        "template_name_column": TEMPLATE_NAME_COLUMN,
        "input_name_column": INPUT_NAME_COLUMN,
        "mappings": mappings,
    }
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    except OSError as exc:
        raise MergeError(f"failed to write mapping sidecar {path}: {exc}") from exc


def validate_mappings(
    mappings: list[dict[str, Any]],
    template_rows: list[dict[str, str]],
    input_rows: list[dict[str, str]],
) -> tuple[dict[int, dict[str, Any]], list[str]]:
    by_template: dict[int, dict[str, Any]] = {}
    used_inputs: dict[int, int] = {}
    deviations: list[str] = []

    for offset, mapping in enumerate(mappings):
        try:
            template_index = int(mapping["template_index"])
            input_index = int(mapping["input_index"])
        except (KeyError, TypeError, ValueError) as exc:
            raise MergeError(f"mapping #{offset} must contain integer template_index and input_index") from exc

        if template_index < 0 or template_index >= len(template_rows):
            raise MergeError(f"mapping #{offset} has invalid template_index {template_index}")
        if input_index < 0 or input_index >= len(input_rows):
            raise MergeError(f"mapping #{offset} has invalid input_index {input_index}")
        if template_index in by_template:
            raise MergeError(f"template row {template_index} is mapped more than once")
        if input_index in used_inputs:
            deviations.append(
                f"input row {input_index} is reused by template rows {used_inputs[input_index]} and {template_index}"
            )

        template_name = template_rows[template_index][TEMPLATE_NAME_COLUMN]
        input_name = input_rows[input_index][INPUT_NAME_COLUMN]
        stored_template_name = mapping.get("template_name")
        stored_input_name = mapping.get("input_name")
        if stored_template_name and stored_template_name != template_name:
            raise MergeError(
                f"mapping for template row {template_index} names {stored_template_name!r}, "
                f"but the current template has {template_name!r}; rerun with --refresh-mapping"
            )
        if stored_input_name and stored_input_name != input_name:
            raise MergeError(
                f"mapping for input row {input_index} names {stored_input_name!r}, "
                f"but the current input has {input_name!r}; rerun with --refresh-mapping"
            )
        normalized_mapping = dict(mapping)
        normalized_mapping.update(
            {
                "template_index": template_index,
                "template_name": mapping.get("template_name") or template_name,
                "input_index": input_index,
                "input_name": mapping.get("input_name") or input_name,
                "source": mapping.get("source") or "unknown",
            }
        )
        by_template[template_index] = normalized_mapping
        used_inputs[input_index] = template_index

    return by_template, deviations


def call_openrouter(
    template_rows: list[dict[str, str]],
    input_rows: list[dict[str, str]],
    unresolved_template_indexes: list[int],
    unused_input_indexes: list[int],
) -> list[dict[str, Any]]:
    api_key = os.environ.get("OPENROUTER_API_KEY")
    model = os.environ.get("OPENROUTER_API_MODEL")

    template_items = [
        {"index": index, "preset_name": template_rows[index][TEMPLATE_NAME_COLUMN]}
        for index in unresolved_template_indexes
    ]
    input_items = [
        {
            "index": index,
            "preset name": input_rows[index][INPUT_NAME_COLUMN],
        }
        for index in unused_input_indexes
    ]

    messages = [
        {
            "role": "system",
            "content": (
                "You match generated preset slugs to original input preset display names. "
                "Return only valid JSON matching the requested schema. "
                "Use each input index at most once. Do not invent indexes."
            ),
        },
        {
            "role": "user",
            "content": json.dumps(
                {
                    "task": (
                        "Match each template preset to the best input preset when you are confident. "
                        "Leave uncertain template presets unmatched by omitting them."
                    ),
                    "template_presets": template_items,
                    "input_presets": input_items,
                    "output_requirements": {
                        "template_index": "index from template_presets",
                        "input_index": "index from input_presets",
                        "confidence": "number from 0 to 1",
                        "reason": "short explanation",
                    },
                },
                ensure_ascii=False,
            ),
        },
    ]

    schema = {
        "type": "object",
        "properties": {
            "mappings": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "template_index": {"type": "integer"},
                        "input_index": {"type": "integer"},
                        "confidence": {"type": "number"},
                        "reason": {"type": "string"},
                    },
                    "required": ["template_index", "input_index", "confidence", "reason"],
                    "additionalProperties": False,
                },
            }
        },
        "required": ["mappings"],
        "additionalProperties": False,
    }

    payload = {
        "model": model,
        "messages": messages,
        "temperature": 0,
        "response_format": {
            "type": "json_schema",
            "json_schema": {"name": "preset_mapping", "strict": True, "schema": schema},
        },
    }

    request = urllib.request.Request(
        OPENROUTER_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "HTTP-Referer": "https://github.com/",
            "X-Title": "halion preset csv merge",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            response_data = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise MergeError(f"OpenRouter request failed with HTTP {exc.code}: {body}") from exc
    except urllib.error.URLError as exc:
        raise MergeError(f"OpenRouter request failed: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise MergeError(f"OpenRouter returned invalid JSON: {exc}") from exc

    try:
        content = response_data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        raise MergeError("OpenRouter response did not contain choices[0].message.content") from exc

    try:
        parsed = json.loads(content)
    except json.JSONDecodeError as exc:
        raise MergeError(f"OpenRouter message content was not valid JSON: {content}") from exc

    if not isinstance(parsed, dict) or not isinstance(parsed.get("mappings"), list):
        raise MergeError("OpenRouter message content must contain a mappings list")

    llm_mappings: list[dict[str, Any]] = []
    for offset, mapping in enumerate(parsed["mappings"]):
        if not isinstance(mapping, dict):
            raise MergeError("OpenRouter returned a non-object mapping")
        try:
            template_index = int(mapping["template_index"])
            input_index = int(mapping["input_index"])
        except (KeyError, TypeError, ValueError) as exc:
            raise MergeError(
                f"OpenRouter mapping #{offset} must contain integer template_index and input_index"
            ) from exc
        if template_index < 0 or template_index >= len(template_rows):
            raise MergeError(f"OpenRouter mapping #{offset} has invalid template_index {template_index}")
        if input_index < 0 or input_index >= len(input_rows):
            raise MergeError(f"OpenRouter mapping #{offset} has invalid input_index {input_index}")
        llm_mappings.append(
            make_mapping(
                template_index,
                template_rows[template_index][TEMPLATE_NAME_COLUMN],
                input_index,
                input_rows[input_index][INPUT_NAME_COLUMN],
                "llm",
                mapping.get("confidence"),
                str(mapping.get("reason") or ""),
            )
        )
    return llm_mappings


def merge_rows(
    template_fields: list[str],
    template_rows: list[dict[str, str]],
    input_fields: list[str],
    input_rows: list[dict[str, str]],
    mappings_by_template: dict[int, dict[str, Any]],
) -> tuple[list[str], list[dict[str, str]]]:
    appended_fields = [field for field in input_fields if field != INPUT_NAME_COLUMN]
    output_fields = template_fields + appended_fields
    output_rows: list[dict[str, str]] = []
    for template_index, template_row in enumerate(template_rows):
        output_row = {field: template_row.get(field, "") for field in template_fields}
        mapping = mappings_by_template.get(template_index)
        if mapping is None:
            for field in appended_fields:
                output_row[field] = ""
        else:
            input_row = input_rows[int(mapping["input_index"])]
            output_row[TEMPLATE_NAME_COLUMN] = input_row[INPUT_NAME_COLUMN]
            for field in appended_fields:
                output_row[field] = input_row.get(field, "")
        output_rows.append(output_row)
    return output_fields, output_rows


def source_row_label(index: int, row: dict[str, str]) -> str:
    extras = []
    for column in ("bank no.", "preset number (in bank)", "preset classification"):
        if column in row and row[column] != "":
            extras.append(f"{column}={row[column]!r}")
    suffix = f" ({', '.join(extras)})" if extras else ""
    return f"input row {index}: {row[INPUT_NAME_COLUMN]!r}{suffix}"


def print_report(
    template_rows: list[dict[str, str]],
    input_rows: list[dict[str, str]],
    output_rows: list[dict[str, str]],
    mappings_by_template: dict[int, dict[str, Any]],
    duplicate_input_names: dict[str, list[int]],
    normalized_notes: list[str],
    validation_deviations: list[str],
    sidecar: Path,
) -> None:
    normalized_count = sum(1 for mapping in mappings_by_template.values() if mapping.get("source") == "normalized")
    llm_count = sum(1 for mapping in mappings_by_template.values() if mapping.get("source") == "llm")
    matched_template_indexes = set(mappings_by_template)
    used_input_indexes = {int(mapping["input_index"]) for mapping in mappings_by_template.values()}
    unmatched_template_indexes = [
        index for index in range(len(template_rows)) if index not in matched_template_indexes
    ]
    unused_input_indexes = [index for index in range(len(input_rows)) if index not in used_input_indexes]

    print("Merge summary")
    print(f"  template rows: {len(template_rows)}")
    print(f"  input rows:    {len(input_rows)}")
    print(f"  output rows:   {len(output_rows)}")
    print(f"  normalized matches: {normalized_count}")
    print(f"  LLM matches:        {llm_count}")
    print(f"  unmatched template rows: {len(unmatched_template_indexes)}")
    print(f"  unused input rows:       {len(unused_input_indexes)}")
    print(f"  mapping sidecar: {sidecar}")

    print_deviation_section("Duplicate input preset names", duplicate_input_name_lines(duplicate_input_names))
    print_deviation_section("Ambiguous normalized matches", normalized_notes)
    print_deviation_section("Invalid or suspicious mapping deviations", validation_deviations)
    print_deviation_section(
        "Unmatched template rows",
        [
            f"template row {index}: {template_rows[index][TEMPLATE_NAME_COLUMN]!r}"
            for index in unmatched_template_indexes
        ],
    )
    print_deviation_section(
        "Unused input rows",
        [source_row_label(index, input_rows[index]) for index in unused_input_indexes],
    )


def duplicate_input_name_lines(duplicates: dict[str, list[int]]) -> list[str]:
    return [
        f"{name!r} appears at input rows {format_indexes(indexes)}"
        for name, indexes in sorted(duplicates.items())
    ]


def print_deviation_section(title: str, lines: list[str]) -> None:
    if not lines:
        return
    print(f"{title}:")
    for line in lines:
        print(f"  - {line}")


def main() -> int:
    args = parse_args()
    try:
        preflight_environment()
        template_fields, template_rows = read_csv(args.template_csv)
        input_fields, input_rows = read_csv(args.input_csv)
        duplicate_input_names = preflight(
            args.template_csv,
            template_fields,
            template_rows,
            args.input_csv,
            input_fields,
            input_rows,
        )
        sidecar = mapping_sidecar_path(args.output_csv)

        if sidecar.exists() and not args.refresh_mapping:
            sidecar_mappings = load_sidecar(sidecar)
            mappings_by_template, validation_deviations = validate_mappings(
                sidecar_mappings, template_rows, input_rows
            )
            normalized_notes: list[str] = []
        else:
            normalized_mappings, normalized_notes = build_normalized_matches(template_rows, input_rows)
            used_input_indexes = {
                int(mapping["input_index"]) for mapping in normalized_mappings.values()
            }
            unresolved_template_indexes = [
                index for index in range(len(template_rows)) if index not in normalized_mappings
            ]
            unused_input_indexes = [
                index for index in range(len(input_rows)) if index not in used_input_indexes
            ]
            llm_mappings: list[dict[str, Any]] = []
            if unresolved_template_indexes and unused_input_indexes:
                llm_mappings = call_openrouter(
                    template_rows,
                    input_rows,
                    unresolved_template_indexes,
                    unused_input_indexes,
                )
            all_mappings = list(normalized_mappings.values()) + llm_mappings
            mappings_by_template, validation_deviations = validate_mappings(
                all_mappings, template_rows, input_rows
            )
            save_sidecar(sidecar, sorted(mappings_by_template.values(), key=lambda item: item["template_index"]))

        output_fields, output_rows = merge_rows(
            template_fields, template_rows, input_fields, input_rows, mappings_by_template
        )
        write_csv(args.output_csv, output_fields, output_rows)
        print_report(
            template_rows,
            input_rows,
            output_rows,
            mappings_by_template,
            duplicate_input_names,
            normalized_notes,
            validation_deviations,
            sidecar,
        )
        return 0
    except MergeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
