#!/usr/bin/env python3
"""Shared local-Qwen wrapper for repo recon and draft patches.

Targets an OpenAI-compatible LM Studio endpoint by default:
  http://127.0.0.1:1234/v1

Examples:
  py -3 scripts/qwen_local.py models
  py -3 scripts/qwen_local.py ping
  py -3 scripts/qwen_local.py recon --task "Trace terrain shadow draw flow" \
      --file GameOS/gameos/gameos_graphics.cpp --file mclib/txmmgr.cpp
  py -3 scripts/qwen_local.py edit --task "Rename this local helper only" \
      --file scripts/qwen_local.py
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import textwrap
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASE_URL = os.environ.get("MC2_QWEN_BASE_URL", "http://127.0.0.1:1234/v1")
DEFAULT_TIMEOUT_S = float(os.environ.get("MC2_QWEN_TIMEOUT_S", "300"))
DEFAULT_MAX_FILE_BYTES = int(os.environ.get("MC2_QWEN_MAX_FILE_BYTES", str(48 * 1024)))
DEFAULT_MAX_FILES = int(os.environ.get("MC2_QWEN_MAX_FILES", "8"))
DEFAULT_MAX_TOKENS = int(os.environ.get("MC2_QWEN_MAX_TOKENS", "500"))
DEFAULT_REASONING_EFFORT = os.environ.get("MC2_QWEN_REASONING_EFFORT", "low")


def fail(msg: str) -> int:
    print(f"[qwen_local] FAIL: {msg}", file=sys.stderr)
    return 1


def _read_json(url: str, timeout_s: float) -> Any:
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout_s) as resp:
        raw = resp.read().decode("utf-8")
    return json.loads(raw)


def _post_json(url: str, payload: dict[str, Any], timeout_s: float) -> Any:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={
            "Accept": "application/json",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout_s) as resp:
        raw = resp.read().decode("utf-8")
    return json.loads(raw)


def _list_models(base_url: str, timeout_s: float) -> list[str]:
    data = _read_json(f"{base_url.rstrip('/')}/models", timeout_s)
    models = []
    for item in data.get("data", []):
        model_id = item.get("id")
        if isinstance(model_id, str) and model_id:
            models.append(model_id)
    return models


def _resolve_model(base_url: str, requested: str | None, timeout_s: float) -> str:
    if requested:
        return requested
    env_model = os.environ.get("MC2_QWEN_MODEL")
    if env_model:
        return env_model
    models = _list_models(base_url, timeout_s)
    for model_id in models:
        if "qwen" in model_id.casefold():
            return model_id
    if models:
        return models[0]
    raise RuntimeError("no models returned by LM Studio")


def _normalize_repo_path(raw_path: str) -> Path:
    p = Path(raw_path)
    if not p.is_absolute():
        p = ROOT / p
    p = p.resolve()
    try:
        p.relative_to(ROOT)
    except ValueError as exc:
        raise ValueError(f"path escapes worktree: {p}") from exc
    if not p.is_file():
        raise ValueError(f"file not found: {p}")
    return p


def _read_file_block(path: Path, max_bytes: int) -> str:
    raw = path.read_text(encoding="utf-8", errors="replace")
    encoded = raw.encode("utf-8")
    if len(encoded) <= max_bytes:
        body = raw
        truncated = False
    else:
        body = encoded[:max_bytes].decode("utf-8", errors="replace")
        truncated = True
    rel = path.relative_to(ROOT).as_posix()
    suffix = "\n[TRUNCATED]\n" if truncated else "\n"
    return f"=== FILE: {rel} ===\n{body}{suffix}"


def _collect_file_blocks(files: list[str], max_files: int, max_bytes: int) -> tuple[list[str], str]:
    if not files:
        return [], ""
    if len(files) > max_files:
        raise ValueError(f"too many files ({len(files)} > {max_files})")
    normalized = [_normalize_repo_path(item) for item in files]
    rels = [p.relative_to(ROOT).as_posix() for p in normalized]
    blocks = [_read_file_block(path, max_bytes) for path in normalized]
    return rels, "\n".join(blocks)


def _build_recon_prompt(task: str, rels: list[str], file_blocks: str) -> str:
    return textwrap.dedent(
        f"""\
        You are doing read-only repository reconnaissance in the canonical MC2 worktree.

        Worktree:
        {ROOT}

        Task:
        {task}

        Constraints:
        - Recon only. Do not propose code changes unless they are listed as possible edit sites.
        - Use only the provided files and the task text. If evidence is missing, say "unknown".
        - Prefer exact file and symbol references from the provided context.
        - Keep the answer compact and structured for a follow-on implementation pass.

        Output format:
        Return valid JSON with this shape:
        {{
          "files": [{{"path": "", "why": ""}}],
          "symbols": [{{"name": "", "file": "", "role": ""}}],
          "flow": ["step 1", "step 2"],
          "edit_sites": [{{"path": "", "reason": ""}}],
          "risks": ["", ""],
          "unknowns": ["", ""]
        }}

        Provided files:
        {json.dumps(rels, indent=2)}

        File contents:
        {file_blocks}
        """
    )


def _build_edit_prompt(task: str, rels: list[str], file_blocks: str) -> str:
    return textwrap.dedent(
        f"""\
        You are generating a draft patch in the canonical MC2 worktree.

        Worktree:
        {ROOT}

        Task:
        {task}

        Constraints:
        - Modify only the provided files.
        - Do not invent files, symbols, or APIs not present in the provided context.
        - Preserve behavior outside the requested change.
        - Output unified diff only.
        - If the request cannot be completed from the provided context, output:
          NEED_MORE_CONTEXT: <reason>

        Allowed files:
        {json.dumps(rels, indent=2)}

        File contents:
        {file_blocks}
        """
    )


def _chat_completion(
    *,
    base_url: str,
    model: str,
    system_prompt: str | None,
    user_prompt: str,
    temperature: float,
    max_tokens: int,
    reasoning_effort: str | None,
    timeout_s: float,
) -> dict[str, Any]:
    messages: list[dict[str, str]] = []
    if system_prompt:
        messages.append({"role": "system", "content": system_prompt})
    messages.append({"role": "user", "content": user_prompt})
    payload = {
        "model": model,
        "messages": messages,
        "temperature": temperature,
        "max_tokens": max_tokens,
    }
    if reasoning_effort and reasoning_effort != "none":
        payload["reasoning_effort"] = reasoning_effort
    return _post_json(f"{base_url.rstrip('/')}/chat/completions", payload, timeout_s)


def _extract_text(response: dict[str, Any]) -> str:
    choices = response.get("choices")
    if not isinstance(choices, list) or not choices:
        raise RuntimeError("no choices in completion response")
    message = choices[0].get("message", {})
    content = message.get("content", "")
    if not isinstance(content, str):
        raise RuntimeError("response content is not text")
    return content.lstrip()


def _extract_debug_stats(response: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    choices = response.get("choices")
    if isinstance(choices, list) and choices:
        choice0 = choices[0]
        if isinstance(choice0, dict):
            finish_reason = choice0.get("finish_reason")
            if finish_reason is not None:
                out["finish_reason"] = finish_reason
            message = choice0.get("message")
            if isinstance(message, dict):
                content = message.get("content")
                reasoning = message.get("reasoning_content")
                out["content_empty"] = not bool(content)
                out["has_reasoning_content"] = bool(reasoning)
    usage = response.get("usage")
    if isinstance(usage, dict):
        for key in ("prompt_tokens", "completion_tokens", "total_tokens"):
            if key in usage:
                out[key] = usage.get(key)
        details = usage.get("completion_tokens_details")
        if isinstance(details, dict) and "reasoning_tokens" in details:
            out["reasoning_tokens"] = details.get("reasoning_tokens")
    return out


def cmd_models(args: argparse.Namespace) -> int:
    try:
        models = _list_models(args.base_url, args.timeout)
    except urllib.error.URLError as exc:
        return fail(f"models request failed: {exc}")
    except Exception as exc:
        return fail(str(exc))
    print(json.dumps(models, indent=2))
    return 0


def cmd_ping(args: argparse.Namespace) -> int:
    try:
        model = _resolve_model(args.base_url, args.model, args.timeout)
        response = _chat_completion(
            base_url=args.base_url,
            model=model,
            system_prompt="Reply with exactly: QWEN_OK",
            user_prompt="Reply with exactly: QWEN_OK",
            temperature=0.0,
            max_tokens=128,
            reasoning_effort="low",
            timeout_s=args.timeout,
        )
        text = _extract_text(response).strip()
    except urllib.error.URLError as exc:
        return fail(f"ping request failed: {exc}")
    except Exception as exc:
        return fail(str(exc))
    if text != "QWEN_OK":
        return fail(f"unexpected ping reply: {text!r}")
    print(f"QWEN_OK model={model}")
    return 0


def _run_task_mode(args: argparse.Namespace, mode: str) -> int:
    try:
        model = _resolve_model(args.base_url, args.model, args.timeout)
        rels, file_blocks = _collect_file_blocks(args.file, args.max_files, args.max_file_bytes)
        if mode == "recon":
            prompt = _build_recon_prompt(args.task, rels, file_blocks)
            system = "You are a careful codebase reconnaissance assistant."
        else:
            prompt = _build_edit_prompt(args.task, rels, file_blocks)
            system = "You are a careful code-edit drafting assistant."
        response = _chat_completion(
            base_url=args.base_url,
            model=model,
            system_prompt=system,
            user_prompt=prompt,
            temperature=args.temperature,
            max_tokens=args.max_tokens,
            reasoning_effort=args.reasoning_effort,
            timeout_s=args.timeout,
        )
        text = _extract_text(response)
        debug = _extract_debug_stats(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        return fail(f"HTTP {exc.code}: {detail}")
    except urllib.error.URLError as exc:
        return fail(f"request failed: {exc}")
    except Exception as exc:
        return fail(str(exc))

    if args.output:
        out_path = Path(args.output)
        if not out_path.is_absolute():
            out_path = ROOT / out_path
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(text, encoding="utf-8")
    if getattr(args, "debug_meta", False):
        print(json.dumps(debug, indent=2), file=sys.stderr)
    print(text, end="" if text.endswith("\n") else "\n")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Shared local LM Studio/Qwen wrapper for recon and draft edits."
    )
    parser.add_argument(
        "--base-url",
        default=DEFAULT_BASE_URL,
        help="OpenAI-compatible LM Studio base URL (default: %(default)s)",
    )
    parser.add_argument(
        "--model",
        help="Model id. Defaults to MC2_QWEN_MODEL or first qwen-like id from /models.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_S,
        help="HTTP timeout in seconds (default: %(default)s)",
    )

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_models = sub.add_parser("models", help="List models exposed by LM Studio.")
    p_models.set_defaults(func=cmd_models)

    p_ping = sub.add_parser("ping", help="Run a minimal end-to-end completion check.")
    p_ping.set_defaults(func=cmd_ping)

    for name in ("recon", "edit"):
        p_mode = sub.add_parser(name, help=f"Run {name} prompt against local Qwen.")
        p_mode.add_argument("--task", required=True, help="Concrete task for the model.")
        p_mode.add_argument(
            "--file",
            action="append",
            default=[],
            help="Repo-relative or absolute file path. Repeat for multiple files.",
        )
        p_mode.add_argument(
            "--max-file-bytes",
            type=int,
            default=DEFAULT_MAX_FILE_BYTES,
            help="Maximum bytes to include from each file (default: %(default)s)",
        )
        p_mode.add_argument(
            "--max-files",
            type=int,
            default=DEFAULT_MAX_FILES,
            help="Maximum number of files allowed in one request (default: %(default)s)",
        )
        p_mode.add_argument(
            "--temperature",
            type=float,
            default=0.0,
            help="Sampling temperature (default: %(default)s)",
        )
        p_mode.add_argument(
            "--max-tokens",
            type=int,
            default=DEFAULT_MAX_TOKENS,
            help="Maximum completion tokens to request (default: %(default)s)",
        )
        p_mode.add_argument(
            "--reasoning-effort",
            choices=["none", "low", "medium", "high"],
            default=DEFAULT_REASONING_EFFORT,
            help="Reasoning budget hint for LM Studio/OpenAI chat (default: %(default)s)",
        )
        p_mode.add_argument(
            "--output",
            help="Optional path to also write the model output.",
        )
        p_mode.add_argument(
            "--debug-meta",
            action="store_true",
            help="Print response metadata summary to stderr.",
        )
        p_mode.set_defaults(func=lambda args, mode=name: _run_task_mode(args, mode))

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
