"""Exercise the JSONL adapter without starting a paid Claude session."""

import asyncio
import importlib.util
import sys
import types
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch


sdk = types.ModuleType("claude_agent_sdk")
sdk_types = types.ModuleType("claude_agent_sdk.types")


class Message:
    def __init__(self, **values):
        self.__dict__.update(values)


class FakeClient:
    instances = []

    def __init__(self, options):
        self.options = options
        self.prompts = []
        self.interrupted = False
        self.connected = False
        self.responses = []
        self.permission_modes = []
        self.effective_permission_mode = None
        self.__class__.instances.append(self)

    async def connect(self):
        self.connected = True
        # Simulate a resumed CLI retaining an earlier automatic mode until explicitly changed.
        self.effective_permission_mode = "auto" if self.options.resume else self.options.permission_mode

    async def set_permission_mode(self, mode):
        self.permission_modes.append(mode)
        self.effective_permission_mode = mode

    async def disconnect(self):
        self.connected = False

    async def query(self, text):
        self.prompts.append(text)

    async def receive_response(self):
        for response in self.responses:
            yield response

    async def interrupt(self):
        self.interrupted = True


for name in ("AssistantMessage", "ResultMessage", "TextBlock", "ThinkingBlock", "ToolUseBlock", "RateLimitEvent",
             "SystemMessage"):
    setattr(sdk, name, type(name, (Message,), {}))
sdk.StreamEvent = type("StreamEvent", (Message,), {})
sdk_types.StreamEvent = sdk.StreamEvent
sdk_types.PermissionResultAllow = type("PermissionResultAllow", (Message,), {})
sdk_types.PermissionResultDeny = type("PermissionResultDeny", (Message,), {})
sdk.ClaudeAgentOptions = lambda **values: Message(**values)
sdk.ClaudeSDKClient = FakeClient
sys.modules["claude_agent_sdk"] = sdk
sys.modules["claude_agent_sdk.types"] = sdk_types

spec = importlib.util.spec_from_file_location("claude_bridge", Path(__file__).resolve().parents[1] / "claude" / "bridge.py")
bridge_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge_module)


class ClaudeBridgeTest(unittest.IsolatedAsyncioTestCase):
    async def test_reasoning_stream_final_and_history(self):
        events = []
        with patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge("/tmp/project")
            await bridge.connect()
            bridge.client.responses = [
                sdk.StreamEvent(event={"type": "message_start", "message": {"id": "msg-1"}}),
                sdk.StreamEvent(event={"type": "content_block_start", "index": 0,
                                       "content_block": {"type": "thinking", "thinking": ""}}),
                sdk.StreamEvent(event={"type": "content_block_delta", "index": 0,
                                       "delta": {"type": "thinking_delta", "thinking": "First "}}),
                sdk.StreamEvent(event={"type": "content_block_delta", "index": 0,
                                       "delta": {"type": "thinking_delta", "thinking": "thought"}}),
                sdk.AssistantMessage(message_id="msg-1", content=[
                    sdk.ThinkingBlock(thinking="First thought", signature="private-signature"),
                    sdk.TextBlock(text="Answer")]),
                sdk.AssistantMessage(message_id="msg-2", content=[sdk.ThinkingBlock(thinking="Unstreamed", signature="")]),
            ]
            await bridge.run_turn("hi")
        reasoning = [event for event in events if event["type"] == "reasoning"]
        self.assertEqual([(event["id"], event["text"]) for event in reasoning], [
            ("msg-1:0", "First "), ("msg-1:0", "First thought"),
            ("msg-1:0", "First thought"), ("msg-2:0", "Unstreamed")])
        self.assertNotIn("private-signature", str(events))
        self.assertEqual(bridge_module.message_entries(Message(type="assistant", uuid="record", message={
            "id": "msg-1", "content": [{"type": "thinking", "thinking": "First thought", "signature": "hidden"},
                                         {"type": "redacted_thinking", "data": "hidden"}]})),
                         [{"role": "reasoning", "id": "msg-1:0", "text": "First thought"}])

    async def test_per_window_rate_limits_reach_the_jsonl_client(self):
        events = []
        with patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge("/tmp/project")
            await bridge.connect()
            bridge.client.responses = [
                sdk.RateLimitEvent(rate_limit_info=Message(
                    status="allowed", rate_limit_type="five_hour", utilization=None, resets_at=1788465600,
                    raw={"status": "allowed", "unifiedWindows": {
                        "five_hour": {"utilization": 0.24, "resetsAt": 1788465600},
                        "seven_day": {"utilization": 0.13, "resetsAt": 1789005600},
                        "seven_day_opus": {"utilization": 0.07, "resetsAt": 1789005600},
                    }})),
                sdk.ResultMessage(result="Done", is_error=False, terminal_reason="completed", session_id="session-test"),
            ]
            await bridge.handle({"type": "prompt", "text": "hello"})
            await bridge.turn_task
            limits = [event for event in events if event["type"] == "rate_limit"]
            self.assertEqual(limits, [
                {"type": "rate_limit", "limit": "five_hour", "utilization": 0.24,
                 "resetsAt": 1788465600, "status": "allowed"},
                {"type": "rate_limit", "limit": "seven_day", "utilization": 0.13,
                 "resetsAt": 1789005600, "status": ""},
                {"type": "rate_limit", "limit": "seven_day_opus", "utilization": 0.07,
                 "resetsAt": 1789005600, "status": ""},
            ])

    async def test_rate_limits_keep_typed_fallback_and_ignore_invalid_snapshots(self):
        typed = Message(status="rejected", rate_limit_type="five_hour", utilization=1.0, resets_at=1000)
        self.assertEqual(bridge_module.rate_limit_updates(typed), [
            {"type": "rate_limit", "limit": "five_hour", "utilization": 1.0, "resetsAt": 1000, "status": "rejected"}
        ])
        typed.raw = {"unifiedWindows": {
            "seven_day": {"utilization": 0.25, "resetsAt": 2000},
            "seven_day_opus": {"utilization": "unknown", "resetsAt": False, "status": {}},
            "seven_day_sonnet": {"utilization": float("nan"), "resetsAt": float("inf")},
            "unknown_window": {"utilization": 0.5, "resetsAt": 3000},
        }}
        result = bridge_module.rate_limit_updates(typed)
        self.assertEqual(result[0]["status"], "rejected")
        self.assertEqual(result[1], {"type": "rate_limit", "limit": "seven_day", "utilization": 0.25,
                                     "resetsAt": 2000, "status": ""})
        self.assertEqual(len(result), 2)
        for raw in (None, [], {"unifiedWindows": []}, {"unifiedWindows": {"seven_day": None}}):
            typed.raw = raw
            self.assertEqual(len(bridge_module.rate_limit_updates(typed)), 1)
        self.assertEqual(bridge_module.rate_limit_updates(Message(rate_limit_type="overage")), [])

    async def test_lists_all_claude_sessions_without_local_index(self):
        session = Message(session_id="session-1", cwd="/tmp/project", custom_title=None,
                          summary="Project chat", first_prompt="Hello", created_at=1000,
                          last_modified=2000, file_size=1234, git_branch="main", tag="saved")
        with patch.object(sdk, "list_sessions", return_value=[session], create=True) as listing:
            self.assertEqual(bridge_module.list_local_sessions([]), [{
                "id": "session-1", "cwd": "/tmp/project", "title": "Project chat", "createdAt": 1,
                "lastModified": 2, "fileSize": 1234, "customTitle": None,
                "summary": "Project chat", "firstPrompt": "Hello", "gitBranch": "main", "tag": "saved",
            }])
            listing.assert_called_once_with(directory=None)

    async def test_reads_session_tail_as_chat_entries(self):
        messages = [
            Message(type="user", message={"role": "user", "content": "First question"}),
            Message(type="assistant", message={"role": "assistant", "content": [
                {"type": "thinking", "thinking": "hidden"},
                {"type": "text", "text": "Checking"},
                {"type": "tool_use", "name": "Read", "input": {}},
            ]}),
            Message(type="user", message={"role": "user", "content": [{"type": "tool_result", "content": "data"}]}),
            Message(type="assistant", message={"role": "assistant", "content": [{"type": "text", "text": "Done"}]}),
        ]
        with patch.object(sdk, "get_session_messages", return_value=messages, create=True) as reading:
            self.assertEqual(bridge_module.read_session("session-1", "/tmp/project", 2), {
                "type": "history", "total": 5,
                "entries": [{"role": "tool", "text": "Read"}, {"role": "assistant", "text": "Done"}],
            })
            reading.assert_called_once_with("session-1", directory="/tmp/project")

    async def test_glm_uses_zai_credentials_and_keeps_directory(self):
        events = []
        with patch.dict(bridge_module.os.environ, {"ZAI_API_KEY": "test-zai-key", "GLM_MODEL": "glm-test"}), \
             patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge("/tmp/project", "glm")
            await bridge.connect()
            options = bridge.client.options
            self.assertEqual(options.permission_mode, "default")
            self.assertEqual(bridge.client.permission_modes, ["default"])
            self.assertEqual(options.model, "glm-test")
            self.assertEqual(options.env["ANTHROPIC_AUTH_TOKEN"], "test-zai-key")
            self.assertEqual(options.env["ANTHROPIC_BASE_URL"], "https://api.z.ai/api/anthropic")
            self.assertEqual(options.setting_sources, ["project", "local"])
            extra_directory = str(Path(__file__).resolve().parent)
            await bridge.handle({"type": "add_directory", "path": extra_directory})
            self.assertEqual(bridge.client.options.add_dirs, [extra_directory])
            self.assertEqual(events[-1], {"type": "directory_added", "path": extra_directory})

    async def test_permission_mode_is_explicit_and_preserved_on_resume(self):
        with patch.object(bridge_module, "send"):
            bridge = bridge_module.Bridge("/tmp/project")
            await bridge.connect()
            self.assertEqual(bridge.client.options.permission_mode, "default")
            self.assertEqual(bridge.client.permission_modes, ["default"])
            bridge.session_id = "existing-auto-session"
            await bridge.handle({"type": "reset_permissions"})
            self.assertEqual(bridge.client.options.resume, "existing-auto-session")
            self.assertEqual(bridge.client.options.permission_mode, "default")
            self.assertEqual(bridge.client.permission_modes, ["default"])
            await bridge.handle({"type": "settings", "readOnly": True})
            self.assertEqual(bridge.client.permission_modes, ["default", "plan"])
            await bridge.handle({"type": "reset_permissions"})
            self.assertEqual(bridge.client.options.permission_mode, "plan")
            self.assertEqual(bridge.client.permission_modes, ["plan"])
            await bridge.handle({"type": "settings", "readOnly": False})
            self.assertEqual(bridge.client.permission_modes, ["plan", "default"])

    async def test_resume_replaces_saved_auto_mode_before_reporting_ready(self):
        with TemporaryDirectory() as directory:
            for read_only, expected_mode in ((False, "default"), (True, "plan")):
                with self.subTest(read_only=read_only):
                    modes_when_ready = []
                    bridge = bridge_module.Bridge(directory, read_only=read_only)

                    def collect(message):
                        if message["type"] == "ready":
                            modes_when_ready.append(bridge.client.effective_permission_mode)

                    with patch.object(bridge_module, "send", collect):
                        await bridge.connect()
                        previous_client = bridge.client
                        previous_client.effective_permission_mode = "auto"
                        modes_when_ready.clear()
                        await bridge.handle({"type": "resume", "session_id": "saved-auto-session", "cwd": directory})
                    self.assertFalse(previous_client.connected)
                    self.assertIsNot(bridge.client, previous_client)
                    self.assertEqual(bridge.client.options.resume, "saved-auto-session")
                    self.assertEqual(bridge.client.options.permission_mode, expected_mode)
                    self.assertEqual(bridge.client.effective_permission_mode, expected_mode)
                    self.assertEqual(modes_when_ready, [expected_mode])
                    self.assertTrue(bridge.connected)

    async def test_resume_does_not_report_ready_if_mode_change_fails(self):
        events = []
        with TemporaryDirectory() as directory, patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge(directory)
            await bridge.connect()
            events.clear()

            async def reject_mode(client, mode):
                raise RuntimeError("permission mode rejected")

            with patch.object(FakeClient, "set_permission_mode", reject_mode):
                with self.assertRaisesRegex(RuntimeError, "permission mode rejected"):
                    await bridge.handle({"type": "resume", "session_id": "saved-auto-session", "cwd": directory})
            self.assertFalse(bridge.connected)
            self.assertFalse(any(event["type"] == "ready" for event in events))
            self.assertEqual(bridge.client.prompts, [])

    async def test_approval_suggestions_cannot_enable_automatic_modes(self):
        events = []
        with patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge("/tmp/project")
            for mode in ("auto", "acceptEdits", "bypassPermissions"):
                context = Message(suggestions=[Message(type="setMode", mode=mode)])
                approval = asyncio.create_task(bridge.can_use_tool("Bash", {"command": "git add ."}, context))
                await asyncio.sleep(0)
                request = events[-1]
                self.assertFalse(request["canRemember"])
                self.assertEqual(request["alwaysRule"], "")
                await bridge.handle({"type": "approval_response", "id": request["id"], "decision": "acceptAlways"})
                self.assertIsNone((await approval).updated_permissions)

    async def test_glm_requires_zai_credentials(self):
        with patch.dict(bridge_module.os.environ, {}, clear=True):
            bridge = bridge_module.Bridge("/tmp/project", "glm")
            with self.assertRaisesRegex(RuntimeError, "ZAI_API_KEY"):
                await bridge.connect()

    async def test_stream_approval_and_new_conversation(self):
        events = []
        with patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge("/tmp/project")
            await bridge.connect()
            client = bridge.client
            client.responses = [
                sdk.StreamEvent(event={"type": "content_block_delta", "delta": {"type": "text_delta", "text": "Hello"}}),
                sdk.AssistantMessage(content=[sdk.TextBlock(text="Hello"), sdk.ToolUseBlock(name="Read", input={"file_path": "a"})]),
                sdk.ResultMessage(result="Hello", is_error=False, terminal_reason="completed", session_id="session-test"),
            ]
            await bridge.handle({"type": "prompt", "text": "hi"})
            await bridge.turn_task
            self.assertEqual(client.prompts, ["hi"])
            self.assertEqual([event["text"] for event in events if event["type"] == "delta"], ["Hello"])
            self.assertEqual([event["name"] for event in events if event["type"] == "tool"], ["Read"])
            self.assertEqual(events[-1]["status"], "completed")

            approval = asyncio.create_task(bridge.can_use_tool("Bash", {"command": "pwd"}, None))
            await asyncio.sleep(0)
            request = events[-1]
            self.assertEqual(request["type"], "approval")
            await bridge.handle({"type": "approval_response", "id": request["id"], "allow": True})
            self.assertIsInstance(await approval, sdk_types.PermissionResultAllow)

            extra_directory = str(Path(__file__).resolve().parent)
            await bridge.handle({"type": "add_directory", "path": extra_directory})
            self.assertFalse(client.connected)
            self.assertEqual(bridge.client.options.add_dirs, [extra_directory])
            self.assertEqual(bridge.client.options.resume, "session-test")
            self.assertEqual(events[-1], {"type": "directory_added", "path": extra_directory})

            await bridge.handle({"type": "new"})
            self.assertFalse(client.connected)
            self.assertIsNot(bridge.client, client)
            self.assertTrue(bridge.client.connected)
            self.assertEqual(bridge.client.options.add_dirs, [extra_directory])
            self.assertIsNone(bridge.client.options.resume)

    async def test_stop_marks_turn_interrupted(self):
        events = []
        with patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge("/tmp/project")
            await bridge.connect()
            waiting = asyncio.Event()

            async def response():
                await waiting.wait()
                yield sdk.ResultMessage(result=None, is_error=False, terminal_reason="aborted")

            bridge.client.receive_response = response
            await bridge.handle({"type": "prompt", "text": "long task"})
            await asyncio.sleep(0)
            await bridge.handle({"type": "stop"})
            waiting.set()
            await bridge.turn_task
            self.assertTrue(bridge.client.interrupted)
            self.assertEqual(events[-1]["status"], "interrupted")


if __name__ == "__main__":
    unittest.main()
