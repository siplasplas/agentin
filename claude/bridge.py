"""JSONL adapter between the Qt client and Claude Agent SDK."""

import argparse
import asyncio
import dataclasses
import json
import os
import sys


def send(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False, default=str) + "\n")
    sys.stdout.flush()


try:
    from claude_agent_sdk import (
        AssistantMessage,
        ClaudeAgentOptions,
        ClaudeSDKClient,
        ResultMessage,
        TextBlock,
        ToolUseBlock,
    )
    from claude_agent_sdk.types import PermissionResultAllow, PermissionResultDeny, StreamEvent
except ImportError as exc:
    send({"type": "error", "message": f"Claude Agent SDK is unavailable: {exc}. Install claude-agent-sdk in the selected Python environment."})
    raise SystemExit(2)

try:
    from claude_agent_sdk import RateLimitEvent
except ImportError:  # SDKs without rate limit events
    RateLimitEvent = None


def describe_permission_updates(updates):
    """Describes the CLI's suggested permission updates for the user, or returns an empty string."""
    parts = []
    for update in updates:
        where = {"localSettings": "this project's local settings", "projectSettings": "this project's settings",
                 "userSettings": "your user settings", "session": "this session"}.get(update.destination or "", "settings")
        if update.type in ("addRules", "replaceRules"):
            rules = ", ".join(rule.tool_name + (f"({rule.rule_content})" if rule.rule_content else "")
                              for rule in update.rules or [])
            parts.append(f"{update.behavior or 'allow'} {rules} in {where}")
        elif update.type == "addDirectories":
            parts.append(f"add {', '.join(update.directories or [])} in {where}")
        elif update.type == "setMode":
            parts.append(f"switch to {update.mode} mode in {where}")
    return "; ".join(parts)


class Bridge:
    def __init__(self, cwd, provider="claude", model=None, effort=None, read_only=False):
        self.cwd = cwd
        self.provider = provider
        self.model = model
        self.effort = effort
        self.read_only = read_only
        self.client = None
        self.turn_task = None
        self.stop_requested = False
        self.pending = {}
        self.next_id = 1
        self.streamed_text = False
        self.turn_had_text = False
        self.session_id = None
        self.additional_dirs = []
        self.connected = False

    async def connect(self, resume=False):
        settings = {
            "cwd": self.cwd,
            "add_dirs": self.additional_dirs,
            "resume": self.session_id if resume else None,
            "include_partial_messages": True,
            "can_use_tool": self.can_use_tool,
            "permission_mode": "plan" if self.read_only else "default",
        }
        if self.provider == "claude":
            if self.model:
                settings["model"] = self.model
            if self.effort:
                settings["effort"] = self.effort
        if self.provider == "glm":
            api_key = os.environ.get("ZAI_API_KEY")
            if not api_key:
                raise RuntimeError("Set ZAI_API_KEY to use GLM through the Z.AI Coding Plan")
            model = self.model or os.environ.get("GLM_MODEL", "glm-5.3")
            settings.update({
                "model": model,
                "setting_sources": ["project", "local"],
                "env": {
                    "ANTHROPIC_AUTH_TOKEN": api_key,
                    "ANTHROPIC_API_KEY": "",
                    "ANTHROPIC_BASE_URL": "https://api.z.ai/api/anthropic",
                    "ANTHROPIC_DEFAULT_OPUS_MODEL": model,
                    "ANTHROPIC_DEFAULT_SONNET_MODEL": model,
                    "ANTHROPIC_DEFAULT_HAIKU_MODEL": model,
                    "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
                },
            })
        options = ClaudeAgentOptions(**settings)
        self.client = ClaudeSDKClient(options=options)
        self.connected = False
        await self.client.connect()
        # Reassert the application mode before accepting prompts, including resumed sessions.
        await self.client.set_permission_mode("plan" if self.read_only else "default")
        self.connected = True
        send({"type": "ready"})

    async def can_use_tool(self, tool_name, input_data, context):
        request_id = self.next_id
        self.next_id += 1
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        # Permission suggestions must not switch the session into auto, acceptEdits, or bypass mode.
        # Mode changes belong to the application's Read-only control, not remembered tool approvals.
        suggestions = [update for update in getattr(context, "suggestions", None) or []
                       if update.type != "setMode"]
        if tool_name == "AskUserQuestion":
            send({"type": "question", "id": request_id, "questions": input_data.get("questions", [])})
        else:
            send({"type": "approval", "id": request_id, "tool": tool_name, "input": input_data,
                  "canRemember": bool(suggestions), "alwaysRule": describe_permission_updates(suggestions)})
        try:
            answer = await future
        finally:
            self.pending.pop(request_id, None)

        if tool_name == "AskUserQuestion":
            if not answer.get("accepted", False):
                return PermissionResultDeny(message="User cancelled the question")
            return PermissionResultAllow(updated_input={
                "questions": input_data.get("questions", []),
                "answers": answer.get("answers", {}),
            })
        decision = answer.get("decision") or ("accept" if answer.get("allow", False) else "decline")
        if decision in ("accept", "acceptForSession", "acceptAlways"):
            # Allowing for the session applies the CLI's suggested rules only to this session; allowing
            # always keeps the destination the CLI suggested, such as the project's local settings.
            updates = None
            if decision == "acceptForSession" and suggestions:
                updates = [dataclasses.replace(update, destination="session") for update in suggestions]
            elif decision == "acceptAlways" and suggestions:
                updates = suggestions
            return PermissionResultAllow(updated_input=input_data, updated_permissions=updates)
        return PermissionResultDeny(message="User declined this action", interrupt=decision == "cancel")

    def resolve_pending(self):
        for future in self.pending.values():
            if not future.done():
                future.set_result({"allow": False, "accepted": False})

    async def run_turn(self, text):
        self.stop_requested = False
        self.streamed_text = False
        self.turn_had_text = False
        status = "completed"
        details = ""
        try:
            await self.client.query(text)
            async for message in self.client.receive_response():
                if isinstance(message, StreamEvent):
                    event = message.event
                    if event.get("type") == "content_block_delta":
                        delta = event.get("delta", {})
                        if delta.get("type") == "text_delta":
                            chunk = delta.get("text", "")
                            if chunk:
                                send({"type": "delta", "text": chunk})
                                self.streamed_text = True
                                self.turn_had_text = True
                elif isinstance(message, AssistantMessage):
                    for block in message.content:
                        if isinstance(block, TextBlock):
                            if not self.streamed_text and block.text:
                                send({"type": "delta", "text": block.text})
                                self.turn_had_text = True
                        elif isinstance(block, ToolUseBlock):
                            send({"type": "tool", "name": block.name, "input": block.input})
                    self.streamed_text = False
                elif RateLimitEvent is not None and isinstance(message, RateLimitEvent):
                    info = message.rate_limit_info
                    send({"type": "rate_limit", "limit": info.rate_limit_type, "utilization": info.utilization,
                          "resetsAt": info.resets_at, "status": info.status})
                elif isinstance(message, ResultMessage):
                    usage = getattr(message, "usage", None)
                    if usage or getattr(message, "total_cost_usd", None) is not None:
                        send({"type": "usage", "usage": usage or {}, "costUsd": message.total_cost_usd})
                    if getattr(message, "session_id", None):
                        self.session_id = message.session_id
                        send({"type": "session", "id": self.session_id})
                    if message.result and not self.turn_had_text:
                        send({"type": "delta", "text": message.result})
                    if self.stop_requested or (message.terminal_reason or "").startswith("aborted"):
                        status = "interrupted"
                    elif message.is_error:
                        status = "failed"
                        details = message.result or "; ".join(message.errors or []) or message.subtype
        except Exception as exc:
            status = "failed"
            details = str(exc)
            send({"type": "error", "message": f"{self.provider.upper()} turn failed: {exc}"})
        finally:
            self.resolve_pending()
            self.turn_task = None
            send({"type": "complete", "status": status, "details": details})

    async def handle(self, command):
        kind = command.get("type")
        if kind == "prompt":
            if self.turn_task is not None:
                send({"type": "error", "message": f"{self.provider.upper()} is already responding"})
                return
            text = command.get("text", "").strip()
            if text:
                self.turn_task = asyncio.create_task(self.run_turn(text))
        elif kind == "stop":
            if self.turn_task is not None:
                self.stop_requested = True
                self.resolve_pending()
                try:
                    await self.client.interrupt()
                except Exception as exc:
                    send({"type": "error", "message": f"Could not interrupt {self.provider.upper()}: {exc}"})
        elif kind == "new":
            if self.turn_task is not None:
                send({"type": "error", "message": f"Wait for {self.provider.upper()} to finish before starting a new conversation"})
                return
            await self.client.disconnect()
            self.connected = False
            self.session_id = None
            requested_cwd = command.get("cwd")
            if requested_cwd:
                path = os.path.realpath(requested_cwd)
                if not os.path.isdir(path):
                    send({"type": "error", "message": f"Directory does not exist: {path}"})
                    await self.connect()
                    return
                self.cwd = path
                self.additional_dirs = []
            await self.connect()
        elif kind == "resume":
            if self.turn_task is not None:
                send({"type": "error", "message": f"Wait for {self.provider.upper()} to finish before resuming a conversation"})
                return
            session_id = command.get("session_id")
            requested_cwd = command.get("cwd")
            if not isinstance(session_id, str) or not session_id:
                send({"type": "error", "message": "Missing session ID"})
                return
            if not isinstance(requested_cwd, str) or not os.path.isdir(requested_cwd):
                send({"type": "error", "message": f"Directory does not exist: {requested_cwd}"})
                return
            await self.client.disconnect()
            self.connected = False
            self.session_id = session_id
            self.cwd = os.path.realpath(requested_cwd)
            self.additional_dirs = []
            await self.connect(resume=True)
        elif kind == "add_directory":
            if self.turn_task is not None:
                send({"type": "error", "message": f"Wait for {self.provider.upper()} to finish before adding a directory"})
                return
            raw_path = command.get("path")
            path = os.path.realpath(raw_path) if isinstance(raw_path, str) and raw_path else ""
            if not os.path.isdir(path):
                send({"type": "error", "message": f"Directory does not exist: {path}"})
                send({"type": "ready"})
                return
            if path in self.additional_dirs or path == os.path.realpath(self.cwd):
                send({"type": "directory_added", "path": path})
                return
            self.additional_dirs.append(path)
            await self.client.disconnect()
            self.connected = False
            await self.connect(resume=True)
            send({"type": "directory_added", "path": path})
        elif kind == "reset_permissions":
            # Rules allowed for the session live in the CLI process; a new connection starts without them.
            if self.turn_task is not None:
                send({"type": "error", "message": f"Wait for {self.provider.upper()} to finish before withdrawing approvals"})
                return
            await self.client.disconnect()
            self.connected = False
            await self.connect(resume=self.session_id is not None)
        elif kind == "settings":
            # The model can change within a session; the effort is a connection option, so changing
            # it reconnects and resumes the session.
            if self.turn_task is not None:
                send({"type": "error", "message": f"Wait for {self.provider.upper()} to finish before changing the model"})
                return
            model = command.get("model") or None
            effort = command.get("effort") or None
            read_only = bool(command.get("readOnly", self.read_only))
            if read_only != self.read_only:
                # Plan mode lets Claude read and plan but not change files; it switches within the session.
                self.read_only = read_only
                await self.client.set_permission_mode("plan" if read_only else "default")
            # GLM maps every Claude model name to its model in the connection's environment.
            if effort != self.effort or (self.provider == "glm" and model != self.model):
                self.model = model
                self.effort = effort
                await self.client.disconnect()
                self.connected = False
                await self.connect(resume=self.session_id is not None)
            elif model != self.model:
                self.model = model
                await self.client.set_model(model)
        elif kind in ("approval_response", "question_response"):
            future = self.pending.get(command.get("id"))
            if future is not None and not future.done():
                future.set_result(command)
        else:
            send({"type": "error", "message": f"Unknown bridge command: {kind}"})


async def read_commands(queue):
    while True:
        line = await asyncio.to_thread(sys.stdin.readline)
        if not line:
            await queue.put({"type": "shutdown"})
            return
        try:
            command = json.loads(line)
            if not isinstance(command, dict):
                raise ValueError("Expected a JSON object")
            await queue.put(command)
        except (json.JSONDecodeError, ValueError) as exc:
            send({"type": "error", "message": f"Invalid JSON command: {exc}"})


async def main(cwd, provider="claude", model=None, effort=None, read_only=False):
    bridge = Bridge(cwd, provider, model, effort, read_only)
    queue = asyncio.Queue()
    reader = asyncio.create_task(read_commands(queue))
    try:
        await bridge.connect()
        while True:
            command = await queue.get()
            if command.get("type") == "shutdown":
                break
            try:
                await bridge.handle(command)
            except Exception as exc:
                send({"type": "error", "message": f"{provider.upper()} bridge error: {exc}"})
                if not bridge.connected:
                    return 1
    except Exception as exc:
        send({"type": "error", "message": f"Could not connect to {provider.upper()}: {exc}"})
        return 1
    finally:
        bridge.resolve_pending()
        if bridge.turn_task is not None:
            bridge.turn_task.cancel()
            try:
                await bridge.turn_task
            except asyncio.CancelledError:
                pass
        if bridge.client is not None:
            try:
                await bridge.client.disconnect()
            except Exception:
                pass
        reader.cancel()
    return 0


def list_local_sessions(directories):
    from claude_agent_sdk import list_sessions

    def preview(value):
        if not value:
            return None
        text = " ".join(value.split())
        return text[:199] + "…" if len(text) > 200 else text

    sessions = []
    if not directories:
        directories = [None]
    for directory in directories:
        if directory is not None and not os.path.isdir(directory):
            continue
        for session in list_sessions(directory=directory):
            cwd = session.cwd or directory
            if not cwd:
                continue
            created_at = session.created_at or session.last_modified
            sessions.append({
                "id": session.session_id,
                "cwd": cwd,
                "title": preview(session.custom_title or session.summary or session.first_prompt) or session.session_id,
                "createdAt": int(created_at / 1000),
                "lastModified": int(session.last_modified / 1000),
                "fileSize": session.file_size,
                "customTitle": preview(session.custom_title),
                "summary": preview(session.summary),
                "firstPrompt": preview(session.first_prompt),
                "gitBranch": session.git_branch,
                "tag": session.tag,
            })
    return sessions


def message_entries(message):
    """Converts one SDK session message into simple chat entries for the Qt client."""
    payload = message.message if isinstance(message.message, dict) else {}
    content = payload.get("content")
    if isinstance(content, str):
        return [{"role": message.type, "text": content}] if content.strip() else []
    entries = []
    for block in content if isinstance(content, list) else []:
        if not isinstance(block, dict):
            continue
        kind = block.get("type")
        if kind == "text" and block.get("text", "").strip():
            entries.append({"role": message.type, "text": block["text"]})
        elif kind == "tool_use":
            entries.append({"role": "tool", "text": block.get("name", "tool")})
    return entries


def read_session(session_id, directory, limit):
    from claude_agent_sdk import get_session_messages

    entries = []
    for message in get_session_messages(session_id, directory=directory):
        entries.extend(message_entries(message))
    return {"type": "history", "entries": entries[-limit:] if limit > 0 else entries, "total": len(entries)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Claude Agent SDK bridge for agentdeskt")
    parser.add_argument("--cwd", required=True)
    parser.add_argument("--provider", choices=("claude", "glm"), default="claude")
    parser.add_argument("--model")
    parser.add_argument("--effort", choices=("low", "medium", "high", "xhigh", "max"))
    parser.add_argument("--read-only", action="store_true")
    parser.add_argument("--list-sessions", action="store_true")
    parser.add_argument("--directories", default="[]")
    parser.add_argument("--read-session")
    parser.add_argument("--limit", type=int, default=20)
    args = parser.parse_args()
    if args.read_session:
        try:
            send(read_session(args.read_session, args.cwd, args.limit))
        except Exception as exc:
            send({"type": "error", "message": f"Could not read Claude Agent SDK session: {exc}"})
            raise SystemExit(1)
        raise SystemExit(0)
    if args.list_sessions:
        try:
            directories = json.loads(args.directories)
            if not isinstance(directories, list) or not all(isinstance(path, str) for path in directories):
                raise ValueError("Expected a list of directory paths")
            send({"type": "sessions", "sessions": list_local_sessions(directories)})
        except Exception as exc:
            send({"type": "error", "message": f"Could not list Claude Agent SDK sessions: {exc}"})
            raise SystemExit(1)
        raise SystemExit(0)
    raise SystemExit(asyncio.run(main(args.cwd, args.provider, args.model, args.effort, args.read_only)))
