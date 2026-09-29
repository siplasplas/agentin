"""Exercise the JSONL adapter without starting a paid Claude session."""

import asyncio
import importlib.util
import sys
import types
import unittest
from pathlib import Path
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
        self.__class__.instances.append(self)

    async def connect(self):
        self.connected = True

    async def disconnect(self):
        self.connected = False

    async def query(self, text):
        self.prompts.append(text)

    async def receive_response(self):
        for response in self.responses:
            yield response

    async def interrupt(self):
        self.interrupted = True


for name in ("AssistantMessage", "ResultMessage", "TextBlock", "ToolUseBlock"):
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

    async def test_glm_uses_zai_credentials_and_keeps_directory(self):
        events = []
        with patch.dict(bridge_module.os.environ, {"ZAI_API_KEY": "test-zai-key", "GLM_MODEL": "glm-test"}), \
             patch.object(bridge_module, "send", events.append):
            bridge = bridge_module.Bridge("/tmp/project", "glm")
            await bridge.connect()
            options = bridge.client.options
            self.assertEqual(options.model, "glm-test")
            self.assertEqual(options.env["ANTHROPIC_AUTH_TOKEN"], "test-zai-key")
            self.assertEqual(options.env["ANTHROPIC_BASE_URL"], "https://api.z.ai/api/anthropic")
            self.assertEqual(options.setting_sources, ["project", "local"])
            extra_directory = str(Path(__file__).resolve().parent)
            await bridge.handle({"type": "add_directory", "path": extra_directory})
            self.assertEqual(bridge.client.options.add_dirs, [extra_directory])
            self.assertEqual(events[-1], {"type": "directory_added", "path": extra_directory})

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
