import assert from "node:assert/strict";
import { randomUUID } from "node:crypto";
import { rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

import { ChatStore } from "../src/chats/chatStore.js";
import { resolveContextWithChatMemory, resolvePromptWithChatMemory } from "../src/chats/contextMemory.js";
import type { TaskRequest } from "../src/contracts.js";

function makeDbPath(): string {
  return join(tmpdir(), `ue-agent-chat-context-${randomUUID()}.sqlite`);
}

function makeRequest(overrides: Partial<TaskRequest>): TaskRequest {
  return {
    prompt: "move it back on x -200",
    mode: "chat",
    context: {},
    ...overrides
  };
}

test("ChatStore.getLatestSelectionNames reads latest selection context from asked history", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("memory test");
    store.appendAsked(chat.id, "/v1/task/plan", "first", {
      mode: "chat",
      context: { selectionNames: ["Actor_A"] }
    });
    store.appendAsked(chat.id, "/v1/session/next", "no context payload", {});
    store.appendAsked(chat.id, "/v1/task/plan", "second", {
      mode: "chat",
      context: { selection: [{ name: "Actor_B" }] }
    });

    assert.deepEqual(store.getLatestSelectionNames(chat.id), ["Actor_B"]);
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("ChatStore.getLatestAskedPrompts returns latest asked prompts first", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("asked prompts");
    store.appendAsked(chat.id, "/v1/task/plan", "first prompt", {});
    store.appendAsked(chat.id, "/v1/task/plan", "second prompt", {});
    store.appendAsked(chat.id, "/v1/task/plan", "third prompt", {});

    assert.deepEqual(store.getLatestAskedPrompts(chat.id, 2), ["third prompt", "second prompt"]);
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("ChatStore pending write prompt can be set and cleared", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("pending write");
    assert.equal(store.getPendingWritePrompt(chat.id), undefined);
    store.setPendingWritePrompt(chat.id, "move selected actor by x 100");
    assert.equal(store.getPendingWritePrompt(chat.id), "move selected actor by x 100");
    store.clearPendingWritePrompt(chat.id);
    assert.equal(store.getPendingWritePrompt(chat.id), undefined);
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("resolveContextWithChatMemory injects remembered selection for referential prompt", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("memory inject");
    store.appendAsked(chat.id, "/v1/task/plan", "move selected actor", {
      mode: "chat",
      context: { selectionNames: ["Actor_1"] }
    });

    const resolved = resolveContextWithChatMemory(
      makeRequest({
        chatId: chat.id,
        context: {},
        prompt: "move it back on x -200"
      }),
      store
    );

    assert.deepEqual(resolved.selectionNames, ["Actor_1"]);
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("resolveContextWithChatMemory keeps explicit current selection", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("do not override");
    store.appendAsked(chat.id, "/v1/task/plan", "old selection", {
      mode: "chat",
      context: { selectionNames: ["Old_Actor"] }
    });

    const resolved = resolveContextWithChatMemory(
      makeRequest({
        chatId: chat.id,
        context: { selectionNames: ["Current_Actor"] },
        prompt: "move it on x 10"
      }),
      store
    );

    assert.deepEqual(resolved.selectionNames, ["Current_Actor"]);
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("resolveContextWithChatMemory does not inject for non-referential prompt", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("non referential");
    store.appendAsked(chat.id, "/v1/task/plan", "old selection", {
      mode: "chat",
      context: { selectionNames: ["Actor_X"] }
    });

    const resolved = resolveContextWithChatMemory(
      makeRequest({
        chatId: chat.id,
        context: {},
        prompt: "create point light at x 0 y 0 z 500"
      }),
      store
    );

    assert.equal(resolved.selectionNames, undefined);
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("resolvePromptWithChatMemory reuses pending write prompt for short ambiguous follow-up", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("follow up yes");
    store.setPendingWritePrompt(chat.id, "sculpt terrain at x 1200 y -300 size 1800 strength 30%");

    const resolvedPrompt = resolvePromptWithChatMemory(
      makeRequest({
        chatId: chat.id,
        prompt: "this is what we need"
      }),
      store
    );

    assert.equal(resolvedPrompt, "sculpt terrain at x 1200 y -300 size 1800 strength 30%");
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("resolvePromptWithChatMemory does not replace explicit question when pending write prompt exists", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("follow up question");
    store.setPendingWritePrompt(chat.id, "set fog density to 0.04");

    const resolvedPrompt = resolvePromptWithChatMemory(
      makeRequest({
        chatId: chat.id,
        prompt: "what is selected right now?"
      }),
      store
    );

    assert.equal(resolvedPrompt, "what is selected right now?");
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("resolvePromptWithChatMemory keeps explicit undo prompt when pending write prompt exists", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("follow up undo");
    store.setPendingWritePrompt(chat.id, "set fog density to 0.04");

    const resolvedPrompt = resolvePromptWithChatMemory(
      makeRequest({
        chatId: chat.id,
        prompt: "undo"
      }),
      store
    );

    assert.equal(resolvedPrompt, "undo");
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("resolvePromptWithChatMemory keeps explicit redo prompt when pending write prompt exists", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("follow up redo");
    store.setPendingWritePrompt(chat.id, "set fog density to 0.04");

    const resolvedPrompt = resolvePromptWithChatMemory(
      makeRequest({
        chatId: chat.id,
        prompt: "redo"
      }),
      store
    );

    assert.equal(resolvedPrompt, "redo");
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("ChatStore.listDetails includes provider, model, and chat type", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("history metadata");
    store.appendAsked(chat.id, "/v1/task/plan", "first prompt", {
      provider: "openai",
      model: "gpt-4.1",
      mode: "agent"
    });

    const details = store.listDetails(chat.id);
    assert.equal(details.length, 1);
    assert.equal(details[0]?.provider, "openai");
    assert.equal(details[0]?.model, "gpt-4.1");
    assert.equal(details[0]?.chatType, "agent");
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("ChatStore.listDetails stores provider, model, and chat type for done entries", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("done metadata");
    store.appendAsked(chat.id, "/v1/task/plan", "user prompt", {
      provider: "gemini",
      model: "gemini-2.5-pro",
      mode: "chat"
    });
    store.appendDone(chat.id, "/v1/task/plan", "assistant answer", {
      displayRole: "assistant",
      displayText: "done"
    });

    const details = store.listDetails(chat.id);
    assert.equal(details.length, 2);
    const doneEntry = details.find((item) => item.kind === "done");
    assert.ok(doneEntry);
    assert.equal(doneEntry.provider, "gemini");
    assert.equal(doneEntry.model, "gemini-2.5-pro");
    assert.equal(doneEntry.chatType, "chat");
  } finally {
    rmSync(dbPath, { force: true });
  }
});

test("ChatStore.listDetails returns latest entries when limit is smaller than history size", () => {
  const dbPath = makeDbPath();
  const store = new ChatStore(dbPath);

  try {
    const chat = store.createChat("history limit");
    for (let index = 0; index < 4; index += 1) {
      store.appendAsked(chat.id, "/v1/task/plan", `ask ${index + 1}`, {
        displayRole: "user",
        displayText: `ask ${index + 1}`
      });
      store.appendDone(chat.id, "/v1/task/plan", `done ${index + 1}`, {
        displayRole: "assistant",
        displayText: `done ${index + 1}`
      });
    }

    const details = store.listDetails(chat.id, 3);
    assert.equal(details.length, 3);
    assert.deepEqual(
      details.map((entry) => entry.summary),
      ["done 3", "ask 4", "done 4"]
    );
  } finally {
    rmSync(dbPath, { force: true });
  }
});
