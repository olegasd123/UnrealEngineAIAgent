import type { TaskRequest } from "../contracts.js";
import { ChatStore } from "./chatStore.js";

function isReferentialPrompt(prompt: string): boolean {
  return /\b(it|them|that|those|selected|selection|same|previous)\b/i.test(prompt);
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
