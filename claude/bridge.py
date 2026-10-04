"""JSONL adapter between the Qt client and Claude Agent SDK."""

import argparse
import asyncio
import dataclasses
import json
import os
import re
import shutil
import sys
import tempfile
import uuid


def send(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False, default=str) + "\n")
    sys.stdout.flush()


try:
    from claude_agent_sdk import (
        AssistantMessage,
        ClaudeAgentOptions,
        ClaudeSDKClient,
        ResultMessage,
        SystemMessage,
        TextBlock,
        ThinkingBlock,
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

try:
    from claude_agent_sdk import HookMatcher
except ImportError:  # SDKs without hooks; agentin's rules then answer approvals only
    HookMatcher = None


# File editing tools and the input field with the edited file.
EDIT_TOOLS = {"Edit": "file_path", "MultiEdit": "file_path", "Write": "file_path", "NotebookEdit": "notebook_path"}

# Tools that only read files, which agentin answers without a question unless they read a secret file.
READ_TOOLS = "Read|Glob|Grep|NotebookRead"

# Appended to Claude Code's system prompt: edits made with these tools can be approved by their path.
EDIT_GUIDANCE = ("Change files with the Edit, MultiEdit and Write tools, not with shell commands such as scripts, "
                 "sed or output redirection. Edits of files in the working directory and the allowed directories "
                 "are approved automatically; shell commands that change files usually need the user's approval.")


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


def rate_limit_updates(info):
    """Normalize typed SDK limits and optional per-window raw snapshots."""
    windows = {"five_hour", "seven_day", "seven_day_opus", "seven_day_sonnet"}
    statuses = {"allowed", "allowed_warning", "rejected"}

    def utilization(value):
        if isinstance(value, (int, float)) and not isinstance(value, bool) and 0 <= value <= 1:
            return value
        return None

    def reset(value):
        if (isinstance(value, (int, float)) and not isinstance(value, bool)
                and 0 < value <= 2**63 - 1 and int(value) == value):
            return int(value)
        return None

    def valid_status(value):
        return value if isinstance(value, str) and value in statuses else ""

    updates = {}
    kind = getattr(info, "rate_limit_type", None)
    if isinstance(kind, str) and kind in windows:
        status = valid_status(getattr(info, "status", ""))
        updates[kind] = {"type": "rate_limit", "limit": kind,
                         "utilization": utilization(getattr(info, "utilization", None)),
                         "resetsAt": reset(getattr(info, "resets_at", None)),
                         "status": status}
    raw = getattr(info, "raw", None)
    snapshots = raw.get("unifiedWindows") if isinstance(raw, dict) else None
    if isinstance(snapshots, dict):
        for window, snapshot in snapshots.items():
            if window not in windows or not isinstance(snapshot, dict):
                continue
            used = utilization(snapshot.get("utilization"))
            resets_at = reset(snapshot.get("resetsAt"))
            status = valid_status(snapshot.get("status"))
            if used is None and resets_at is None and not status:
                continue
            # A rejection of the top-level window does not reject unrelated windows.
            update = updates.setdefault(window, {"type": "rate_limit", "limit": window,
                                                "utilization": None, "resetsAt": None, "status": ""})
            if used is not None:
                update["utilization"] = used
            if resets_at is not None:
                update["resetsAt"] = resets_at
            if status:
                update["status"] = status
    return list(updates.values())


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
        # Directories allowed for this CLI session by an approved suggestion; a reconnect forgets them.
        self.session_dirs = []
        # A steering message was sent during the turn and may start another CLI turn after its result.
        self.steered = False
        self.connected = False

    def settings_directories(self):
        """Returns the additionalDirectories saved in the Claude Code settings this session reads."""
        files = [os.path.join(self.cwd, ".claude", "settings.json"), os.path.join(self.cwd, ".claude", "settings.local.json")]
        if self.provider == "claude":
            config = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.expanduser("~/.claude")
            files.append(os.path.join(config, "settings.json"))
        directories = []
        for path in files:
            try:
                with open(path, encoding="utf-8") as file:
                    data = json.load(file)
            except (OSError, ValueError):
                continue
            permissions = data.get("permissions") if isinstance(data, dict) else None
            entries = permissions.get("additionalDirectories") if isinstance(permissions, dict) else None
            directories.extend(entry for entry in entries or [] if isinstance(entry, str) and entry)
        return directories

    def writable_directories(self):
        """The chat's directory, the directories allowed for the session or in settings, and the temporary
        directory, which agents use for scratch files as Codex does."""
        roots = [self.cwd] + self.additional_dirs + self.session_dirs + self.settings_directories()
        directories = []
        for root in roots + ["/tmp", tempfile.gettempdir()]:
            root = os.path.realpath(os.path.join(self.cwd, os.path.expanduser(root)))
            if root not in directories:
                directories.append(root)
        return directories

    def report_writable(self):
        """Tells agentin the directories besides the chat's own where this session may write, so that its
        directory lock holds them; the temporary directory is shared by everyone and is left out."""
        shared = {os.path.realpath("/tmp"), os.path.realpath(tempfile.gettempdir()), os.path.realpath(self.cwd)}
        send({"type": "writable", "directories": [path for path in self.writable_directories() if path not in shared]})

    def path_is_allowed(self, path):
        """Whether path is in one of the writable directories."""
        target = os.path.realpath(os.path.join(self.cwd, os.path.expanduser(path)))
        return any(os.path.commonpath([target, root]) == root for root in self.writable_directories())

    def edit_is_allowed(self, tool_name, input_data):
        """Edits in the allowed directories need no question."""
        key = EDIT_TOOLS.get(tool_name)
        target = input_data.get(key) if key and isinstance(input_data, dict) else None
        return not self.read_only and isinstance(target, str) and bool(target) and self.path_is_allowed(target)

    async def connect(self, resume=False):
        self.session_dirs = []
        settings = {
            "cwd": self.cwd,
            "add_dirs": self.additional_dirs,
            "resume": self.session_id if resume else None,
            "include_partial_messages": True,
            "can_use_tool": self.can_use_tool,
            "permission_mode": "plan" if self.read_only else "default",
            "system_prompt": {"type": "preset", "preset": "claude_code", "append": EDIT_GUIDANCE},
        }
        if self.provider == "claude":
            if self.model:
                settings["model"] = self.model
            if self.effort:
                settings["effort"] = self.effort
        if self.provider == "glm":
            model = self.model or os.environ.get("GLM_MODEL", "glm-5.3")
            settings.update({"model": model, "setting_sources": ["project", "local"], "env": glm_environment(model)})
        if HookMatcher is not None:
            # agentin's rules decide on shell commands and file reads before Claude Code applies its own
            # permission rules.
            settings["hooks"] = {"PreToolUse": [HookMatcher(matcher="Bash", hooks=[self.check_command]),
                                                HookMatcher(matcher=READ_TOOLS, hooks=[self.check_read])]}
        options = ClaudeAgentOptions(**settings)
        self.client = ClaudeSDKClient(options=options)
        self.connected = False
        await self.client.connect()
        self.report_writable()
        # Reassert the application mode before accepting prompts, including resumed sessions.
        await self.client.set_permission_mode("plan" if self.read_only else "default")
        self.connected = True
        send({"type": "ready"})

    async def ask_agentin(self, message):
        """Sends a check to agentin and waits for its answer."""
        request_id = self.next_id
        self.next_id += 1
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        send({**message, "id": request_id})
        try:
            return await future
        finally:
            self.pending.pop(request_id, None)

    @staticmethod
    def hook_decision(answer, updated_input=None):
        """The hook's answer for agentin's decision; no decision leaves the tool to Claude Code."""
        decision = answer.get("decision")
        if decision not in ("allow", "deny"):
            return {}
        output = {"hookEventName": "PreToolUse", "permissionDecision": decision,
                  "permissionDecisionReason": answer.get("reason") or "agentin's rules"}
        if decision == "allow" and updated_input is not None:
            output["updatedInput"] = updated_input
        return {"hookSpecificOutput": output}

    async def check_command(self, input_data, tool_use_id, context):
        """Asks agentin whether its rules allow or deny a shell command; no answer leaves it to Claude Code."""
        tool_input = (input_data.get("tool_input") or {}) if isinstance(input_data, dict) else {}
        command = tool_input.get("command")
        if not isinstance(command, str) or not command.strip():
            return {}
        # agentin judges where the command writes: it runs in the session's current directory, and writing is
        # free in the directories where edits are.
        cwd = input_data.get("cwd") if isinstance(input_data.get("cwd"), str) else None
        # How long the command may run: Claude Code's timeout in milliseconds, or none in the background.
        timeout = tool_input.get("timeout") if isinstance(tool_input.get("timeout"), (int, float)) else None
        answer = await self.ask_agentin({"type": "command_check", "command": command, "cwd": cwd or self.cwd,
                                         "writable": self.writable_directories(), "timeout": timeout,
                                         "background": bool(tool_input.get("run_in_background"))})
        # A sed command that agentin's rules allow runs with --sandbox, which rejects the sed commands that
        # read, write or run other files, should agentin's reading of the script miss one.
        updated = None
        if answer.get("sandboxSed") and sys.platform.startswith("linux") and re.match(r"\s*sed\s", command):
            updated = {**tool_input, "command": re.sub(r"^\s*sed\b", "sed --sandbox", command, count=1)}
        return self.hook_decision(answer, updated)

    async def check_read(self, input_data, tool_use_id, context):
        """Asks agentin about a reading tool: it allows reading anywhere, and asks about secret files."""
        tool_input = (input_data.get("tool_input") or {}) if isinstance(input_data, dict) else {}
        tool = input_data.get("tool_name") if isinstance(input_data, dict) else None
        if not isinstance(tool, str) or not isinstance(tool_input, dict):
            return {}
        cwd = input_data.get("cwd") if isinstance(input_data.get("cwd"), str) else None
        answer = await self.ask_agentin({"type": "read_check", "tool": tool, "input": tool_input, "cwd": cwd or self.cwd})
        return self.hook_decision(answer)

    async def can_use_tool(self, tool_name, input_data, context):
        if self.edit_is_allowed(tool_name, input_data):
            return PermissionResultAllow(updated_input=input_data)
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
            # An edit asks only outside the writable directories, or in a read-only chat; agentin says which.
            key = EDIT_TOOLS.get(tool_name)
            target = input_data.get(key) if key and isinstance(input_data, dict) else None
            outside = isinstance(target, str) and bool(target) and not self.path_is_allowed(target)
            send({"type": "approval", "id": request_id, "tool": tool_name, "input": input_data,
                  "canRemember": bool(suggestions), "alwaysRule": describe_permission_updates(suggestions),
                  "outsideWritable": outside, "writable": self.writable_directories() if outside else []})
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
            for update in updates or []:
                if update.type == "addDirectories":
                    self.session_dirs.extend(update.directories or [])
            if any(update.type == "addDirectories" for update in updates or []):
                self.report_writable()
            return PermissionResultAllow(updated_input=input_data, updated_permissions=updates)
        return PermissionResultDeny(message="User declined this action", interrupt=decision == "cancel")

    def resolve_pending(self):
        for future in self.pending.values():
            if not future.done():
                future.set_result({"allow": False, "accepted": False})

    async def turn_messages(self):
        """Yields the messages of a turn, including the CLI turns that steering messages started.

        A steering message that arrives while the model still works joins the running turn. One that arrives
        after its last step makes the CLI answer it in another turn, announced right after the result with a
        "requesting" status; that turn belongs to the same turn of the chat."""
        async for message in self.client.receive_response():
            yield message
        while self.steered:
            self.steered = False
            stream = self.client.receive_response()
            try:
                first = await asyncio.wait_for(anext(stream), timeout=2)
            except asyncio.TimeoutError:
                if self.steered:
                    continue
                return
            except StopAsyncIteration:
                return
            if self.turn_had_text:
                send({"type": "delta", "text": "\n\n"})
            yield first
            async for message in stream:
                yield message

    async def run_turn(self, text):
        self.stop_requested = False
        self.streamed_text = False
        self.turn_had_text = False
        status = "completed"
        details = ""
        thinking_id = str(uuid.uuid4())
        thinking = {}
        try:
            self.steered = False
            await self.client.query(text)
            async for message in self.turn_messages():
                if isinstance(message, StreamEvent):
                    event = message.event
                    if event.get("type") == "message_start":
                        thinking_id = event.get("message", {}).get("id") or str(uuid.uuid4())
                        thinking = {}
                    elif event.get("type") == "content_block_start":
                        block = event.get("content_block", {})
                        if block.get("type") == "thinking":
                            index = event.get("index", 0)
                            thinking[index] = block.get("thinking", "")
                            if thinking[index]:
                                send({"type": "reasoning", "id": f"{thinking_id}:{index}", "text": thinking[index]})
                    elif event.get("type") == "content_block_delta":
                        delta = event.get("delta", {})
                        if delta.get("type") == "thinking_delta":
                            index = event.get("index", 0)
                            thinking[index] = thinking.get(index, "") + delta.get("thinking", "")
                            send({"type": "reasoning", "id": f"{thinking_id}:{index}", "text": thinking[index]})
                        elif delta.get("type") == "text_delta":
                            chunk = delta.get("text", "")
                            if chunk:
                                send({"type": "delta", "text": chunk})
                                self.streamed_text = True
                                self.turn_had_text = True
                elif isinstance(message, AssistantMessage):
                    for index, block in enumerate(message.content):
                        if isinstance(block, TextBlock):
                            if not self.streamed_text and block.text:
                                send({"type": "delta", "text": block.text})
                                self.turn_had_text = True
                        elif isinstance(block, ThinkingBlock):
                            item_id = getattr(message, "message_id", None) or thinking_id
                            send({"type": "reasoning", "id": f"{item_id}:{index}", "text": block.thinking})
                        elif isinstance(block, ToolUseBlock):
                            send({"type": "tool", "name": block.name, "input": block.input})
                    self.streamed_text = False
                    thinking_id = str(uuid.uuid4())
                    thinking = {}
                elif isinstance(message, SystemMessage) and message.subtype == "api_retry":
                    # The CLI retries failed API requests silently for minutes; the server's message is not included.
                    data = message.data or {}
                    send({"type": "retry", "attempt": data.get("attempt"), "maxRetries": data.get("max_retries"),
                          "delayMs": data.get("retry_delay_ms"), "status": data.get("error_status"),
                          "error": data.get("error")})
                elif RateLimitEvent is not None and isinstance(message, RateLimitEvent):
                    for update in rate_limit_updates(message.rate_limit_info):
                        send(update)
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
        elif kind == "steer":
            text = command.get("text")
            if self.turn_task is None or not isinstance(text, str) or not text.strip():
                send({"type": "steer_failed", "text": text if isinstance(text, str) else "",
                      "message": "No turn is running"})
                return
            self.steered = True
            try:
                await self.client.query(text)
            except Exception as exc:
                # agentin waits for an answer to every steering message before it lets the user steer again.
                send({"type": "steer_failed", "text": text, "message": str(exc) or type(exc).__name__})
                return
            send({"type": "steer_accepted", "text": text})
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
        elif kind in ("approval_response", "question_response", "command_check_response"):
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
    for index, block in enumerate(content if isinstance(content, list) else []):
        if not isinstance(block, dict):
            continue
        kind = block.get("type")
        if kind == "text" and block.get("text", "").strip():
            entries.append({"role": message.type, "text": block["text"]})
        elif kind == "thinking" and block.get("thinking", "").strip():
            item_id = payload.get("id") or getattr(message, "uuid", "history")
            entries.append({"role": "reasoning", "id": f"{item_id}:{index}", "text": block["thinking"]})
        elif kind == "tool_use":
            entries.append({"role": "tool", "text": block.get("name", "tool")})
    return entries


def glm_environment(model):
    """Points Claude Code at Z.AI's Anthropic-compatible endpoint, with every model alias on the GLM model."""
    api_key = os.environ.get("ZAI_API_KEY")
    if not api_key:
        raise RuntimeError("Set ZAI_API_KEY to use GLM through the Z.AI Coding Plan")
    return {
        "ANTHROPIC_AUTH_TOKEN": api_key,
        "ANTHROPIC_API_KEY": "",
        "ANTHROPIC_BASE_URL": "https://api.z.ai/api/anthropic",
        "ANTHROPIC_DEFAULT_OPUS_MODEL": model,
        "ANTHROPIC_DEFAULT_SONNET_MODEL": model,
        "ANTHROPIC_DEFAULT_HAIKU_MODEL": model,
        "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC": "1",
    }


async def suggest(prompt, provider):
    """Answers one prompt with a light model and no tools, leaving no session behind.

    It runs in a temporary directory, and the session Claude Code records for that directory is removed
    afterwards, so it never appears among the user's conversations."""
    from claude_agent_sdk import query

    directory = os.path.realpath(tempfile.mkdtemp(prefix="agentin-suggest-"))
    settings = {"cwd": directory, "tools": [], "max_turns": 1, "setting_sources": [], "permission_mode": "default"}
    if provider == "glm":
        # The flash model is Z.AI's cheapest, which is enough for a few short suggestions.
        model = os.environ.get("GLM_SUGGEST_MODEL", "glm-4.7-flash")
        settings.update({"model": model, "env": glm_environment(model)})
    else:
        settings["model"] = "haiku"
    text = ""
    try:
        async for message in query(prompt=prompt, options=ClaudeAgentOptions(**settings)):
            if isinstance(message, AssistantMessage):
                parts = [block.text for block in message.content if isinstance(block, TextBlock)]
                if parts:
                    text = "".join(parts)
            elif isinstance(message, ResultMessage) and message.is_error:
                raise RuntimeError(message.result or "; ".join(message.errors or []) or "the model failed")
    finally:
        shutil.rmtree(directory, ignore_errors=True)
        config = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.expanduser("~/.claude")
        shutil.rmtree(os.path.join(config, "projects", re.sub(r"[^A-Za-z0-9]", "-", directory)), ignore_errors=True)
    return text


def read_session(session_id, directory, limit):
    from claude_agent_sdk import get_session_messages

    entries = []
    for message in get_session_messages(session_id, directory=directory):
        entries.extend(message_entries(message))
    return {"type": "history", "entries": entries[-limit:] if limit > 0 else entries, "total": len(entries)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Claude Agent SDK bridge for agentin")
    parser.add_argument("--cwd", required=True)
    parser.add_argument("--provider", choices=("claude", "glm"), default="claude")
    parser.add_argument("--model")
    parser.add_argument("--effort", choices=("low", "medium", "high", "xhigh", "max"))
    parser.add_argument("--read-only", action="store_true")
    parser.add_argument("--list-sessions", action="store_true")
    parser.add_argument("--directories", default="[]")
    parser.add_argument("--read-session")
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--suggest")
    args = parser.parse_args()
    if args.suggest:
        try:
            send({"type": "suggestions", "text": asyncio.run(suggest(args.suggest, args.provider))})
        except Exception as exc:
            send({"type": "error", "message": f"Could not get suggestions: {exc}"})
            raise SystemExit(1)
        raise SystemExit(0)
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
