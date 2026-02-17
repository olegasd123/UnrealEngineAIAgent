import type { TaskRequest } from "../contracts.js";
import { ChatStore } from "./chatStore.js";

function isReferentialPrompt(prompt: string): boolean {
  return /\b(it|them|that|those|selected|selection|same|previous)\b/i.test(prompt);
}

export function hasWriteIntent(prompt: string): boolean {
  return /\b(move|offset|translate|shift|rotate|turn|spin|scale|resize|grow|shrink|create|spawn|add|build|make|generate|delete|remove|destroy|erase|set|assign|apply|replace|duplicate|copy|clone|paint|sculpt|undo|revert|rollback|roll back|redo|do again|reapply)\b/i.test(
    prompt
  );
}

function hasExplicitNonWriteIntent(prompt: string): boolean {
  const normalized = prompt.trim();
  if (normalized.length === 0) {
    return false;
  }
  if (/\?/.test(normalized)) {
    return true;
  }
  return /^(what|which|show|list|describe|summarize|summary|get|read|inspect|help|why|how|when|where)\b/i.test(normalized);
}

function hasSelectionInContext(context: TaskRequest["context"]): boolean {
  const selection = Array.isArray(context.selection) ? context.selection : [];
  const selectionNames = Array.isArray(context.selectionNames) ? context.selectionNames : [];
  return selection.length > 0 || selectionNames.length > 0;
}

export function resolveContextWithChatMemory(
  input: Pick<TaskRequest, "prompt" | "context" | "chatId">,
  chatStore: ChatStore
): TaskRequest["context"] {
  if (!input.chatId) {
    return input.context;
  }
  if (hasSelectionInContext(input.context)) {
    return input.context;
  }
  if (!isReferentialPrompt(input.prompt)) {
    return input.context;
  }

  const rememberedSelectionNames = chatStore.getLatestSelectionNames(input.chatId);
  if (rememberedSelectionNames.length === 0) {
    return input.context;
  }

  return {
    ...input.context,
    selectionNames: rememberedSelectionNames
  };
}

export function resolvePromptWithChatMemory(
  input: Pick<TaskRequest, "prompt" | "chatId">,
  chatStore: ChatStore
): string {
  const currentPrompt = input.prompt.trim();
  if (!input.chatId || currentPrompt.length === 0) {
    return input.prompt;
  }

  if (hasWriteIntent(currentPrompt)) {
    return input.prompt;
  }

  if (hasExplicitNonWriteIntent(currentPrompt)) {
    return input.prompt;
  }

  const tokenCount = currentPrompt.split(/\s+/).filter((item) => item.length > 0).length;
  if (tokenCount > 12 || currentPrompt.length > 120) {
    return input.prompt;
  }

  const pendingWritePrompt = chatStore.getPendingWritePrompt(input.chatId);
  if (pendingWritePrompt) {
    return pendingWritePrompt;
  }

  return input.prompt;
}
