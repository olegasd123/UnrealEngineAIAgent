import type { ProviderName } from "../config.js";

export interface ModelRef {
  provider: ProviderName;
  model: string;
}

export function normalizeModelRef(input: ModelRef): ModelRef | undefined {
  const model = input.model.trim();
  if (!model) {
    return undefined;
  }

  return { provider: input.provider, model };
}
