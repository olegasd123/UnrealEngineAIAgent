import type { PlanOutput } from "../contracts.js";
import type { LlmProvider } from "../providers/types.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";
import { buildRuleBasedPlan } from "./ruleBasedPlanner.js";
import { WorldStateCollector } from "../worldState/worldStateCollector.js";

function isWriteAction(plan: PlanOutput): boolean {
  return plan.actions.some((action) => action.command !== "context.getSceneSummary" && action.command !== "context.getSelection");
}

function hasWriteIntent(prompt: string): boolean {
  return /\b(move|offset|translate|shift|rotate|turn|spin|scale|resize|grow|shrink|create|spawn|add|build|make|generate|delete|remove|destroy|erase|set|assign|apply|replace|duplicate|copy|clone|paint|sculpt|place|put|attach|drop|undo|revert|rollback|roll back|redo|do again|reapply)\b/i.test(
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

function inferRuntimeGrassTemplateFromPrompt(prompt: string): string | undefined {
  const lower = prompt.toLowerCase();
  if (!/(pcg|procedural content generation)/.test(lower)) {
    return undefined;
  }
  if (!/(create|make|new|build|generate)/.test(lower)) {
    return undefined;
  }

  const hasRuntimeGrassGpuHint =
    /(runtime\s+grass\s+gpu|runtime\s+gpu\s+grass)/.test(lower) || /tpl_showcase_runtimegrassgpu/.test(lower);
  const hasGrassHint = /\bgrass\b/.test(lower);
  const hasTemplateHint = /\b(template|built[\s-]*in)\b/.test(lower);
  const hasTemplatePhraseHint = /\b(?:from|form|using)\s+(?:built[\s-]*in\s+)?template\b/.test(lower);

  if (hasRuntimeGrassGpuHint || (hasGrassHint && (hasTemplateHint || hasTemplatePhraseHint))) {
    return "/PCG/GraphTemplates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU";
  }

  return undefined;
}

function enrichPcgCreateGraphFromFallback(providerPlan: PlanOutput, fallbackPlan: PlanOutput, prompt: string): boolean {
  const fallbackPcgCreateGraphAction = fallbackPlan.actions.find((action) => action.command === "pcg.createGraph");
  const fallbackTemplatePath =
    fallbackPcgCreateGraphAction?.command === "pcg.createGraph"
      ? fallbackPcgCreateGraphAction.params.templatePath?.trim()
      : undefined;
  const inferredTemplatePath = fallbackTemplatePath || inferRuntimeGrassTemplateFromPrompt(prompt);
  if (!inferredTemplatePath) {
    return false;
  }

  let changed = false;
  for (const action of providerPlan.actions) {
    if (action.command !== "pcg.createGraph") {
      continue;
    }

    const existingTemplatePath = action.params.templatePath?.trim();
    if (!existingTemplatePath) {
      action.params.templatePath = inferredTemplatePath;
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
        const enrichmentNotes: string[] = [];
        if (enrichLandscapeGenerateFromFallback(providerPlan, fallback)) {
          enrichmentNotes.push("Filled missing landscape.generate constraints from prompt parsing.");
        }
        if (enrichPcgCreateGraphFromFallback(providerPlan, fallback, intent.prompt)) {
          enrichmentNotes.push("Filled missing pcg.createGraph template from prompt parsing.");
        }

        if (enrichmentNotes.length > 0) {
          return {
            ...providerPlan,
            steps: [...enrichmentNotes, ...providerPlan.steps]
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
