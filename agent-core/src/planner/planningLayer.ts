import type { PlanOutput } from "../contracts.js";
import type { LlmProvider } from "../providers/types.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";
import { WorldStateCollector } from "../worldState/worldStateCollector.js";

function isWriteAction(plan: PlanOutput): boolean {
  return plan.actions.some((action) => action.command !== "context.getSceneSummary" && action.command !== "context.getSelection");
}

function hasWriteIntent(prompt: string): boolean {
  return /\b(move|offset|translate|shift|rotate|turn|spin|scale|resize|grow|shrink|create|spawn|add|delete|remove|destroy|erase|set|assign|apply|replace|duplicate|copy|clone|paint|sculpt|undo|revert|rollback|roll back|redo|do again|reapply)\b/i.test(
    prompt
  );
}

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
      const providerPlan = await provider.planTask(planInput);
      if (providerPlan.actions.length > 0) {
        return providerPlan;
      }

      const fallback = buildRuleBasedPlan(intent.input, {
        goalType: intent.goalType,
        constraints: intent.constraints,
        successCriteria: intent.successCriteria
      });
      if (hasWriteIntent(intent.prompt) && isWriteAction(fallback)) {
        return {
          ...fallback,
          steps: ["Provider returned no executable actions for a write intent. Using local fallback.", ...fallback.steps]
        };
      }

      return providerPlan;
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
