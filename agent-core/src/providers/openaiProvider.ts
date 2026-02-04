import type { ProviderRuntimeConfig } from "../config.js";
import type { PlanOutput } from "../contracts.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";
import type { LlmProvider, PlanInput } from "./types.js";

export class OpenAiProvider implements LlmProvider {
  public readonly name = "openai";
  public readonly adapter = "stub";
  public readonly model: string;
  public readonly hasApiKey: boolean;

  constructor(private readonly runtime: ProviderRuntimeConfig) {
    this.model = runtime.model;
    this.hasApiKey = Boolean(runtime.apiKey);
  }

  async planTask(input: PlanInput): Promise<PlanOutput> {
    const basePlan = buildRuleBasedPlan(input);
    const setupStep = this.hasApiKey
      ? `OpenAI adapter stub is active (model: ${this.model}).`
      : "OPENAI_API_KEY is missing. Using local parser stub.";

    return {
      ...basePlan,
      steps: [setupStep, ...basePlan.steps]
    };
  }
}
