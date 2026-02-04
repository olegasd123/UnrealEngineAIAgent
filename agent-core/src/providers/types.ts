import type { PlanOutput, TaskRequest } from "../contracts.js";
import type { ProviderName, ProviderRuntimeConfig } from "../config.js";

export type PlanInput = TaskRequest;

export interface ProviderFactoryConfig {
  selected: ProviderName;
  openai: ProviderRuntimeConfig;
  gemini: ProviderRuntimeConfig;
}

export interface LlmProvider {
  name: ProviderName;
  model: string;
  hasApiKey: boolean;
  adapter: "stub" | "live";
  planTask(input: PlanInput): Promise<PlanOutput>;
}
