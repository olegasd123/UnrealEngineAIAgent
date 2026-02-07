import type { PlanOutput } from "../contracts.js";
import type { LlmProvider } from "../providers/types.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";
import { WorldStateCollector } from "../worldState/worldStateCollector.js";

export class PlanningLayer {
  constructor(private readonly worldStateCollector: WorldStateCollector = new WorldStateCollector()) {}

  async buildPlan(intent: NormalizedIntent, provider: LlmProvider): Promise<PlanOutput> {
    const worldState = this.worldStateCollector.collect(intent);
    const planInput = {
      request: intent.input,
      normalizedPrompt: intent.prompt,
      goalType: intent.goalType,
      constraints: intent.constraints,
      successCriteria: intent.successCriteria,
      worldState
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
