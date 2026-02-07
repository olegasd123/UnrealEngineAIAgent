import type { PlanOutput, TaskRequest } from "../contracts.js";
import type { ProviderName, ProviderRuntimeConfig } from "../config.js";
import type { GoalType } from "../intent/intentLayer.js";

export interface PlanInput {
  request: TaskRequest;
  normalizedPrompt: string;
  goalType: GoalType;
  constraints: string[];
  successCriteria: string[];
}

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
