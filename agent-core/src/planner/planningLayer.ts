import type { PlanOutput } from "../contracts.js";
import type { LlmProvider } from "../providers/types.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";

export class PlanningLayer {
  async buildPlan(intent: NormalizedIntent, provider: LlmProvider): Promise<PlanOutput> {
    try {
      return await provider.planTask(intent.input);
    } catch (error) {
      const reason = error instanceof Error ? error.message : "Unknown provider error";
      const fallback = buildRuleBasedPlan(intent.input);
      return {
        ...fallback,
        steps: [`Provider planning failed. Using local fallback. Reason: ${reason}`, ...fallback.steps]
      };
    }
  }
}
