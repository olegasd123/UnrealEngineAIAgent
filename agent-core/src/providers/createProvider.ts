import { GeminiProvider } from "./geminiProvider.js";
import { LocalProvider } from "./localProvider.js";
import { OpenAiProvider } from "./openaiProvider.js";
import type { LlmProvider, ProviderFactoryConfig } from "./types.js";

export function createProvider(config: ProviderFactoryConfig): LlmProvider {
  if (config.selected === "local") {
    return new LocalProvider(config.local);
  }

  if (config.selected === "gemini") {
    return new GeminiProvider(config.gemini);
  }

  return new OpenAiProvider(config.openai);
}
