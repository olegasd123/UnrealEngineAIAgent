import { PlanOutputSchema, type PlanOutput } from "../contracts.js";
import type { NormalizedIntent } from "../intent/intentLayer.js";

export interface ValidationResult {
  plan: PlanOutput;
  notes: string[];
}

export class ValidationLayer {
  validatePlan(intent: NormalizedIntent, candidate: unknown): ValidationResult {
    const plan = PlanOutputSchema.parse(candidate);
    const notes: string[] = [];

    if (plan.actions.length === 0) {
      notes.push("No actions parsed from intent.");
    }
    if (intent.goalType === "scene_delete" && plan.actions.every((action) => action.command !== "scene.deleteActor")) {
      notes.push("Delete intent detected, but delete action was not planned.");
    }

    return { plan, notes };
  }
}
