import { PlanOutputSchema, type PlanOutput } from "../contracts.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";

export interface ValidationResult {
  plan: PlanOutput;
  notes: string[];
}

function uniqueStrings(values: string[]): string[] {
  return Array.from(new Set(values.map((value) => value.trim()).filter((value) => value.length > 0)));
}

function readContextSelectionNames(intent: NormalizedIntent): string[] {
  const names: string[] = [];
  const context = intent.input.context as Record<string, unknown>;

  const selectionNames = context.selectionNames;
  if (Array.isArray(selectionNames)) {
    for (const item of selectionNames) {
      if (typeof item === "string") {
        names.push(item);
      }
    }
  }

  const selection = context.selection;
  if (Array.isArray(selection)) {
    for (const item of selection) {
      if (typeof item === "string") {
        names.push(item);
        continue;
      }
      if (item && typeof item === "object") {
        const record = item as Record<string, unknown>;
        if (typeof record.name === "string") {
          names.push(record.name);
        }
      }
    }
  }

  return uniqueStrings(names);
}

function toByNameIfSelectionTargeted(plan: PlanOutput, actorNames: string[]): number {
  if (actorNames.length === 0) {
    return 0;
  }

  let rewrites = 0;
  for (const action of plan.actions) {
    if ("target" in action.params && action.params.target === "selection") {
      action.params.target = "byName";
      (action.params as { actorNames?: string[] }).actorNames = actorNames;
      rewrites += 1;
    }
  }

  return rewrites;
}

function normalizeRisk(action: PlanOutput["actions"][number]): boolean {
  if (
    action.command === "session.beginTransaction" ||
    action.command === "session.commitTransaction" ||
    action.command === "session.rollbackTransaction"
  ) {
    if (action.risk !== "low") {
      action.risk = "low";
      return true;
    }
    return false;
  }

  if (action.command === "scene.modifyActor") {
    if (action.risk !== "low") {
      action.risk = "low";
      return true;
    }
    return false;
  }

  if (action.command === "scene.modifyComponent") {
    const hasVisibilityOnlyChange =
      action.params.visibility !== undefined &&
      !action.params.deltaLocation &&
      !action.params.deltaRotation &&
      !action.params.deltaScale &&
      !action.params.scale;
    if (hasVisibilityOnlyChange && action.risk !== "low") {
      action.risk = "low";
      return true;
    }
  }

  return false;
}

export class ValidationLayer {
  validatePlan(intent: NormalizedIntent, candidate: unknown): ValidationResult {
    const plan = PlanOutputSchema.parse(candidate);
    const notes: string[] = [];
    const rememberedSelectionNames = readContextSelectionNames(intent);
    const rewrittenActions = toByNameIfSelectionTargeted(plan, rememberedSelectionNames);
    let normalizedRiskActions = 0;
    for (const action of plan.actions) {
      if (normalizeRisk(action)) {
        normalizedRiskActions += 1;
      }
    }

    if (plan.actions.length === 0) {
      notes.push("No actions parsed from intent.");
    }
    if (rewrittenActions > 0) {
      notes.push(`Rewrote ${rewrittenActions} selection-targeted action(s) to byName using context selection.`);
    }
    if (normalizedRiskActions > 0) {
      notes.push(`Normalized risk to low for ${normalizedRiskActions} safe action(s).`);
    }
    if (!plan.goal.id || !plan.goal.description) {
      notes.push("Plan goal is incomplete.");
    }
    if (plan.subgoals.length === 0) {
      notes.push("Plan has no subgoals. Consider adding milestones.");
    }
    if (plan.checks.length === 0) {
      notes.push("Plan has no checks. Consider adding validation checks.");
    }
    if (plan.stopConditions.length === 0) {
      notes.push("Plan has no stop conditions.");
    }
    if (intent.goalType === "scene_delete" && plan.actions.every((action) => action.command !== "scene.deleteActor")) {
      notes.push("Delete intent detected, but delete action was not planned.");
    }

    return { plan, notes };
  }
}
