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
    action.command === "context.getSceneSummary" ||
    action.command === "context.getSelection" ||
    action.command === "editor.undo" ||
    action.command === "editor.redo" ||
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

  if (
    action.command === "scene.setDirectionalLightIntensity" ||
    action.command === "scene.setFogDensity" ||
    action.command === "scene.setPostProcessExposureCompensation"
  ) {
    if (action.risk !== "low") {
      action.risk = "low";
      return true;
    }
    return false;
  }

  return false;
}

function isSceneWriteAction(action: PlanOutput["actions"][number]): boolean {
  return (
    action.command === "scene.modifyActor" ||
    action.command === "scene.createActor" ||
    action.command === "scene.deleteActor" ||
    action.command === "scene.modifyComponent" ||
    action.command === "scene.setComponentMaterial" ||
    action.command === "scene.setComponentStaticMesh" ||
    action.command === "scene.addActorTag" ||
    action.command === "scene.setActorFolder" ||
    action.command === "scene.addActorLabelPrefix" ||
    action.command === "scene.duplicateActors" ||
    action.command === "scene.setDirectionalLightIntensity" ||
    action.command === "scene.setFogDensity" ||
    action.command === "scene.setPostProcessExposureCompensation" ||
    action.command === "landscape.sculpt" ||
    action.command === "landscape.paintLayer" ||
    action.command === "landscape.generate" ||
    action.command === "editor.undo" ||
    action.command === "editor.redo"
  );
}

function isSessionTransactionAction(action: PlanOutput["actions"][number]): boolean {
  return (
    action.command === "session.beginTransaction" ||
    action.command === "session.commitTransaction" ||
    action.command === "session.rollbackTransaction"
  );
}

function addInternalTransactionIfNeeded(intent: NormalizedIntent, plan: PlanOutput): boolean {
  if (intent.input.mode !== "agent") {
    return false;
  }

  const writeIndexes = plan.actions
    .map((action, index) => ({ action, index }))
    .filter((entry) => isSceneWriteAction(entry.action))
    .map((entry) => entry.index);

  // For single write action, per-action transaction in plugin is enough.
  if (writeIndexes.length < 2) {
    return false;
  }

  const firstWriteIndex = writeIndexes[0]!;
  const lastWriteIndex = writeIndexes[writeIndexes.length - 1]!;

  let changed = false;

  const hasBeginBeforeWrite = plan.actions.some(
    (action, index) => action.command === "session.beginTransaction" && index <= firstWriteIndex
  );
  if (!hasBeginBeforeWrite) {
    plan.actions.splice(firstWriteIndex, 0, {
      command: "session.beginTransaction",
      params: { description: "UE AI Agent Session" },
      risk: "low"
    });
    changed = true;
  }

  const writeIndexShift = changed ? 1 : 0;
  const normalizedLastWriteIndex = lastWriteIndex + writeIndexShift;
  const hasCommitAfterWrite = plan.actions.some(
    (action, index) => action.command === "session.commitTransaction" && index >= normalizedLastWriteIndex
  );
  if (!hasCommitAfterWrite) {
    plan.actions.splice(normalizedLastWriteIndex + 1, 0, {
      command: "session.commitTransaction",
      params: {},
      risk: "low"
    });
    changed = true;
  }

  // Remove rollback from normal execution flow. Rollback is for failure handling.
  const filtered = plan.actions.filter((action) => action.command !== "session.rollbackTransaction");
  if (filtered.length !== plan.actions.length) {
    plan.actions = filtered;
    changed = true;
  }

  return changed;
}

function normalizeContextOnlySummary(plan: PlanOutput): boolean {
  if (plan.actions.length === 0) {
    return false;
  }

  const isContextOnly = plan.actions.every(
    (action) => action.command === "context.getSceneSummary" || action.command === "context.getSelection"
  );
  if (!isContextOnly) {
    return false;
  }

  const hasSceneSummary = plan.actions.some((action) => action.command === "context.getSceneSummary");
  const hasSelection = plan.actions.some((action) => action.command === "context.getSelection");

  const normalizedSummary = hasSceneSummary && hasSelection
    ? "Collect current scene and selection context."
    : hasSelection
    ? "Collect current selection context."
    : "Collect current scene summary context.";

  if (plan.summary === normalizedSummary) {
    return false;
  }

  plan.summary = normalizedSummary;
  return true;
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
    const insertedInternalTransaction = addInternalTransactionIfNeeded(intent, plan);
    const normalizedContextSummary = normalizeContextOnlySummary(plan);

    if (plan.actions.length === 0) {
      notes.push("No actions parsed from intent.");
    }
    if (rewrittenActions > 0) {
      notes.push(`Rewrote ${rewrittenActions} selection-targeted action(s) to byName using context selection.`);
    }
    if (normalizedRiskActions > 0) {
      notes.push(`Normalized risk to low for ${normalizedRiskActions} safe action(s).`);
    }
    if (insertedInternalTransaction) {
      notes.push("Added internal transaction wrappers for multi-action agent plan.");
    }
    if (normalizedContextSummary) {
      notes.push("Normalized summary text for context-only plan.");
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
    if (
      plan.actions.some(isSceneWriteAction) &&
      !plan.actions.some(isSessionTransactionAction) &&
      intent.input.mode === "agent" &&
      plan.actions.filter(isSceneWriteAction).length >= 2
    ) {
      notes.push("Multi-action agent plan is missing internal transaction wrappers.");
    }

    return { plan, notes };
  }
}
