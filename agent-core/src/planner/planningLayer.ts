import type { PlanOutput } from "../contracts.js";
import type { LlmProvider } from "../providers/types.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";
import { WorldStateCollector } from "../worldState/worldStateCollector.js";

function isWriteAction(plan: PlanOutput): boolean {
  return plan.actions.some((action) => action.command !== "context.getSceneSummary" && action.command !== "context.getSelection");
}

function hasWriteIntent(prompt: string): boolean {
  return /\b(move|offset|translate|shift|rotate|turn|spin|scale|resize|grow|shrink|create|spawn|add|build|make|generate|delete|remove|destroy|erase|set|assign|apply|replace|duplicate|copy|clone|paint|sculpt|undo|revert|rollback|roll back|redo|do again|reapply)\b/i.test(
    prompt
  );
}

function enrichLandscapeGenerateFromFallback(providerPlan: PlanOutput, fallbackPlan: PlanOutput): boolean {
  const fallbackLandscapeAction = fallbackPlan.actions.find((action) => action.command === "landscape.generate");
  if (!fallbackLandscapeAction || fallbackLandscapeAction.command !== "landscape.generate") {
    return false;
  }

  let changed = false;
  for (const action of providerPlan.actions) {
    if (action.command !== "landscape.generate") {
      continue;
    }

    if (action.params.theme !== "nature_island" || fallbackLandscapeAction.params.theme !== "nature_island") {
      continue;
    }

    if (action.params.mountainStyle === undefined && fallbackLandscapeAction.params.mountainStyle !== undefined) {
      action.params.mountainStyle = fallbackLandscapeAction.params.mountainStyle;
      changed = true;
    }
    if (action.params.mountainCount === undefined && fallbackLandscapeAction.params.mountainCount !== undefined) {
      action.params.mountainCount = fallbackLandscapeAction.params.mountainCount;
      changed = true;
    }
    if (action.params.mountainWidthMin === undefined && fallbackLandscapeAction.params.mountainWidthMin !== undefined) {
      action.params.mountainWidthMin = fallbackLandscapeAction.params.mountainWidthMin;
      changed = true;
    }
    if (action.params.mountainWidthMax === undefined && fallbackLandscapeAction.params.mountainWidthMax !== undefined) {
      action.params.mountainWidthMax = fallbackLandscapeAction.params.mountainWidthMax;
      changed = true;
    }
    if (action.params.maxHeight === undefined && fallbackLandscapeAction.params.maxHeight !== undefined) {
      action.params.maxHeight = fallbackLandscapeAction.params.maxHeight;
      changed = true;
    }
  }

  return changed;
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
      const writeIntent = hasWriteIntent(intent.prompt);
      const fallback = buildRuleBasedPlan(intent.input, {
        goalType: intent.goalType,
        constraints: intent.constraints,
        successCriteria: intent.successCriteria
      });

      if (providerPlan.actions.length > 0 && writeIntent && !isWriteAction(providerPlan) && isWriteAction(fallback)) {
        return {
          ...fallback,
          steps: ["Provider returned context-only actions for a write intent. Using local fallback.", ...fallback.steps]
        };
      }

      if (providerPlan.actions.length > 0) {
        const enriched = enrichLandscapeGenerateFromFallback(providerPlan, fallback);
        if (enriched) {
          return {
            ...providerPlan,
            steps: ["Filled missing landscape.generate constraints from prompt parsing.", ...providerPlan.steps]
          };
        }
        return providerPlan;
      }

      if (writeIntent && isWriteAction(fallback)) {
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
