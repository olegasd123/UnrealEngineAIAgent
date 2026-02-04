import type { ProviderRuntimeConfig } from "../config.js";
import type { PlanOutput } from "../contracts.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";
import type { LlmProvider, PlanInput } from "./types.js";

export class GeminiProvider implements LlmProvider {
  public readonly name = "gemini";
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
      ? `Gemini adapter stub is active (model: ${this.model}).`
      : "GEMINI_API_KEY is missing. Using local parser stub.";

    return {
      ...basePlan,
      steps: [setupStep, ...basePlan.steps]
    };
  }
}
