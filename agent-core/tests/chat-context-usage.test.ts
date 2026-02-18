import assert from "node:assert/strict";
import test from "node:test";

import { estimateContextUsageFromChatDetails } from "../src/chats/contextUsage.js";
import type { ChatDetailEntry } from "../src/chats/chatStore.js";

function makeDetail(overrides: Partial<ChatDetailEntry>): ChatDetailEntry {
  return {
    id: "detail-1",
    chatId: "chat-1",
    kind: "asked",
    route: "/v1/task/plan",
    summary: "move selected actor by x 100",
    createdAt: "2026-01-01T00:00:00.000Z",
    ...overrides
  };
}

test("estimateContextUsageFromChatDetails uses latest chat provider/model metadata", () => {
  const details: ChatDetailEntry[] = [
    makeDetail({
      provider: "openai",
      model: "gpt-4o-mini"
    }),
    makeDetail({
      id: "detail-2",
      kind: "done",
      summary: "Done.",
      provider: "local",
      model: "qwen2.5-7b-32k"
    })
  ];

  const usage = estimateContextUsageFromChatDetails(
    details,
    {
      openai: 128_000,
      gemini: 1_048_576,
      local: 8_192
    }
  );

  assert.ok(usage);
  assert.equal(usage?.provider, "local");
  assert.equal(usage?.model, "qwen2.5-7b-32k");
  assert.equal(usage?.contextWindowTokens, 32_000);
});

test("estimateContextUsageFromChatDetails includes context payload and prompt text", () => {
  const details: ChatDetailEntry[] = [
    makeDetail({
      payload: {
        displayText: "move it on x 100",
        context: {
          selectionNames: ["SM_Cube_1", "SM_Cube_2", "SM_Cube_3"]
        }
      }
    }),
    makeDetail({
      id: "detail-2",
      kind: "done",
      summary: "Moved 3 actors.",
      payload: {
        displayText: "Moved 3 actors."
      }
    })
  ];

  const usage = estimateContextUsageFromChatDetails(
    details,
    {
      openai: 128_000,
      gemini: 1_048_576,
      local: 8_192
    },
    "openai",
    "gpt-4o"
  );

  assert.ok(usage);
  assert.ok((usage?.usedTokens ?? 0) > 0);
  assert.ok((usage?.usedPercent ?? 0) > 0);
  assert.equal(usage?.status, "ok");
});
