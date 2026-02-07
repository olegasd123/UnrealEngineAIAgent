import type { PlanOutput } from "../contracts.js";
import type { LlmProvider } from "../providers/types.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";

export class PlanningLayer {
  async buildPlan(intent: NormalizedIntent, provider: LlmProvider): Promise<PlanOutput> {
    const planInput = {
      request: intent.input,
      normalizedPrompt: intent.prompt,
      goalType: intent.goalType,
      constraints: intent.constraints,
      successCriteria: intent.successCriteria
    };

    try {
      return await provider.planTask(planInput);
    } catch (error) {
      const reason = error instanceof Error ? error.message : "Unknown provider error";
      const fallback = buildRuleBasedPlan(intent.input, {
        goalType: intent.goalType,
        constraints: intent.constraints,
        successCriteria: intent.successCriteria
      });
      return {
        ...fallback,
        steps: [`Provider planning failed. Using local fallback. Reason: ${reason}`, ...fallback.steps]
      };
    }
  }
}
