"""JSONL adapter between the Qt client and Claude Agent SDK."""

import argparse
import asyncio
import json
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


class Bridge:
    def __init__(self, cwd):
        self.cwd = cwd
        self.client = None
        self.turn_task = None
        self.stop_requested = False
        self.pending = {}
        self.next_id = 1
        self.streamed_text = False
        self.turn_had_text = False

    async def connect(self):
        options = ClaudeAgentOptions(
            cwd=self.cwd,
            include_partial_messages=True,
            can_use_tool=self.can_use_tool,
        )
        self.client = ClaudeSDKClient(options=options)
        await self.client.connect()
        send({"type": "ready"})

    async def can_use_tool(self, tool_name, input_data, context):
        request_id = self.next_id
        self.next_id += 1
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        if tool_name == "AskUserQuestion":
            send({"type": "question", "id": request_id, "questions": input_data.get("questions", [])})
        else:
            send({"type": "approval", "id": request_id, "tool": tool_name, "input": input_data})
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
        if answer.get("allow", False):
            return PermissionResultAllow(updated_input=input_data)
        return PermissionResultDeny(message="User declined this action")

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
                elif isinstance(message, ResultMessage):
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
            send({"type": "error", "message": f"Claude turn failed: {exc}"})
        finally:
            self.resolve_pending()
            self.turn_task = None
            send({"type": "complete", "status": status, "details": details})

    async def handle(self, command):
        kind = command.get("type")
        if kind == "prompt":
            if self.turn_task is not None:
                send({"type": "error", "message": "Claude is already responding"})
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
                    send({"type": "error", "message": f"Could not interrupt Claude: {exc}"})
        elif kind == "new":
            if self.turn_task is not None:
                send({"type": "error", "message": "Wait for Claude to finish before starting a new conversation"})
                return
            await self.client.disconnect()
            await self.connect()
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


async def main(cwd):
    bridge = Bridge(cwd)
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
                send({"type": "error", "message": f"Claude bridge error: {exc}"})
    except Exception as exc:
        send({"type": "error", "message": f"Could not connect to Claude: {exc}"})
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
            await bridge.client.disconnect()
        reader.cancel()
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Claude Agent SDK bridge for agentdeskt")
    parser.add_argument("--cwd", required=True)
    args = parser.parse_args()
    raise SystemExit(asyncio.run(main(args.cwd)))
