import type { ProviderName } from "../config.js";
import type { ChatDetailEntry } from "./chatStore.js";

const CHARS_PER_TOKEN = 4;
const MESSAGE_OVERHEAD_TOKENS = 8;
const BASE_SYSTEM_OVERHEAD_TOKENS = 180;

const CONTEXT_NEAR_LIMIT_RATIO = 0.9;
const CONTEXT_FULL_RATIO = 1;

const MODEL_CONTEXT_HINT_PATTERN = /(?:^|[^a-z0-9])(\d+)\s*([km])(?:[^a-z0-9]|$)/gi;
const MODEL_CONTEXT_NUMBER_PATTERN = /(?:ctx|context|window)[^\d]{0,6}(\d{3,7})/gi;

export interface ContextWindowDefaults {
  openai: number;
  gemini: number;
  local: number;
}

export interface ContextUsageSnapshot {
  provider: ProviderName;
  model: string;
  usedTokens: number;
  contextWindowTokens: number;
  remainingTokens: number;
  usedPercent: number;
  status: "ok" | "near_limit" | "full" | "overflow";
  estimated: true;
}

function normalizeProvider(value: unknown): ProviderName | undefined {
  if (typeof value !== "string") {
    return undefined;
  }
  const normalized = value.trim().toLowerCase();
  if (normalized === "openai" || normalized === "gemini" || normalized === "local") {
    return normalized;
  }
  return undefined;
}

function estimateTokenCountFromText(text: string): number {
  const normalized = text.trim();
  if (!normalized) {
    return 0;
  }

  const byChars = Math.ceil(normalized.length / CHARS_PER_TOKEN);
  const byWords = Math.ceil(normalized.split(/\s+/).filter((item) => item.length > 0).length * 1.3);
  return Math.max(byChars, byWords, 1);
}

function estimateTokenCountFromUnknown(value: unknown): number {
  if (typeof value === "string") {
    return estimateTokenCountFromText(value);
  }
  try {
    return estimateTokenCountFromText(JSON.stringify(value));
  } catch {
    return 0;
  }
}

function pickDisplayText(detail: ChatDetailEntry): string {
  const payload = detail.payload;
  if (payload && typeof payload === "object") {
    const record = payload as Record<string, unknown>;
    if (typeof record.displayText === "string" && record.displayText.trim().length > 0) {
      return record.displayText;
    }
  }
  return detail.summary;
}

function inferContextWindowFromModel(model: string): number | undefined {
  const normalized = model.trim().toLowerCase();
  if (!normalized) {
    return undefined;
  }

  let bestHint = 0;
  MODEL_CONTEXT_HINT_PATTERN.lastIndex = 0;
  for (let match = MODEL_CONTEXT_HINT_PATTERN.exec(normalized); match; match = MODEL_CONTEXT_HINT_PATTERN.exec(normalized)) {
    const rawValue = Number(match[1]);
    if (!Number.isFinite(rawValue) || rawValue <= 0) {
      continue;
    }
    const unit = match[2];
    const tokens = unit === "m" ? rawValue * 1_000_000 : rawValue * 1_000;
    if (tokens > bestHint) {
      bestHint = tokens;
    }
  }
  if (bestHint > 0) {
    return bestHint;
  }

  let bestNumber = 0;
  MODEL_CONTEXT_NUMBER_PATTERN.lastIndex = 0;
  for (
    let match = MODEL_CONTEXT_NUMBER_PATTERN.exec(normalized);
    match;
    match = MODEL_CONTEXT_NUMBER_PATTERN.exec(normalized)
  ) {
    const rawValue = Number(match[1]);
    if (!Number.isFinite(rawValue) || rawValue <= 0) {
      continue;
    }
    if (rawValue > bestNumber) {
      bestNumber = rawValue;
    }
  }
  if (bestNumber > 0) {
    return bestNumber;
  }

  return undefined;
}

function resolveContextWindowTokens(
  provider: ProviderName,
  model: string,
  defaults: ContextWindowDefaults
): number {
  const inferred = inferContextWindowFromModel(model);
  if (inferred && inferred > 0) {
    return inferred;
  }

  if (provider === "openai") {
    return defaults.openai;
  }
  if (provider === "gemini") {
    return defaults.gemini;
  }
  return defaults.local;
}

function resolveUsageStatus(usedTokens: number, windowTokens: number): ContextUsageSnapshot["status"] {
  if (windowTokens <= 0) {
    return "overflow";
  }

  const ratio = usedTokens / windowTokens;
  if (ratio > CONTEXT_FULL_RATIO) {
    return "overflow";
  }
  if (ratio >= CONTEXT_FULL_RATIO) {
    return "full";
  }
  if (ratio >= CONTEXT_NEAR_LIMIT_RATIO) {
    return "near_limit";
  }
  return "ok";
}

function resolveProviderModel(
  details: ChatDetailEntry[],
  provider?: ProviderName,
  model?: string
): { provider?: ProviderName; model?: string } {
  const normalizedProvider = normalizeProvider(provider);
  const normalizedModel = model?.trim();
  if (normalizedProvider && normalizedModel) {
    return { provider: normalizedProvider, model: normalizedModel };
  }

  for (let index = details.length - 1; index >= 0; index -= 1) {
    const detail = details[index];
    const foundProvider = normalizeProvider(detail.provider);
    const foundModel = detail.model?.trim();
    if (!normalizedProvider && foundProvider) {
      provider = foundProvider;
    }
    if (!normalizedModel && foundModel) {
      model = foundModel;
    }
    if (provider && model) {
      break;
    }
  }

  return {
    provider: normalizeProvider(provider),
    model: model?.trim() || undefined
  };
}

export function estimateContextUsageFromChatDetails(
  details: ChatDetailEntry[],
  defaults: ContextWindowDefaults,
  provider?: ProviderName,
  model?: string
): ContextUsageSnapshot | undefined {
  const resolved = resolveProviderModel(details, provider, model);
  if (!resolved.provider || !resolved.model) {
    return undefined;
  }

  let usedTokens = BASE_SYSTEM_OVERHEAD_TOKENS;
  for (const detail of details) {
    usedTokens += MESSAGE_OVERHEAD_TOKENS;
    usedTokens += estimateTokenCountFromText(pickDisplayText(detail));

    if (detail.kind === "asked" && detail.payload && typeof detail.payload === "object") {
      const payload = detail.payload as Record<string, unknown>;
      if (payload.context !== undefined) {
        usedTokens += estimateTokenCountFromUnknown(payload.context);
      }
      if (payload.resolvedPrompt !== undefined) {
        usedTokens += estimateTokenCountFromUnknown(payload.resolvedPrompt);
      }
    }
  }

  const contextWindowTokens = Math.max(1, resolveContextWindowTokens(resolved.provider, resolved.model, defaults));
  const remainingTokens = Math.max(0, contextWindowTokens - usedTokens);
  const usedPercent = Number(((usedTokens / contextWindowTokens) * 100).toFixed(2));

  return {
    provider: resolved.provider,
    model: resolved.model,
    usedTokens,
    contextWindowTokens,
    remainingTokens,
    usedPercent,
    status: resolveUsageStatus(usedTokens, contextWindowTokens),
    estimated: true
  };
}
