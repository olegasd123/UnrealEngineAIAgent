import type { TaskRequest } from "../contracts.js";

export type GoalType =
  | "scene_transform"
  | "scene_create"
  | "scene_delete"
  | "pcg_graph"
  | "material_style"
  | "lighting_tune"
  | "unknown";

export interface NormalizedIntent {
  input: TaskRequest;
  prompt: string;
  goalType: GoalType;
  constraints: string[];
  successCriteria: string[];
}

function readSelectionCount(context: TaskRequest["context"]): number {
  const names = new Set<string>();
  if (Array.isArray((context as { selectionNames?: unknown }).selectionNames)) {
    for (const item of (context as { selectionNames?: unknown[] }).selectionNames ?? []) {
      if (typeof item === "string" && item.trim().length > 0) {
        names.add(item.trim());
      }
    }
  }
  if (Array.isArray((context as { selection?: unknown }).selection)) {
    for (const item of ((context as { selection?: unknown[] }).selection ?? [])) {
      if (typeof item === "string" && item.trim().length > 0) {
        names.add(item.trim());
        continue;
      }
      if (item && typeof item === "object") {
        const record = item as Record<string, unknown>;
        if (typeof record.name === "string" && record.name.trim().length > 0) {
          names.add(record.name.trim());
        }
      }
    }
  }
  return names.size;
}

function detectGoalType(prompt: string): GoalType {
  const lower = prompt.toLowerCase();
  if (/(pcg|procedural content generation|pcg graph|graph node|surface sampler|transform points)/.test(lower)) {
    return "pcg_graph";
  }
  if (/(set|assign|apply|replace).*(material|shader)|(material|shader).*(style|look)/.test(lower)) {
    return "material_style";
  }
  if (/(light|lighting|exposure|fog|shadow|sky light|directional light)/.test(lower)) {
    return "lighting_tune";
  }
  if (/(delete|remove|destroy|erase)/.test(lower)) {
    return "scene_delete";
  }
  if (/(create|spawn|add)/.test(lower)) {
    return "scene_create";
  }
  if (/(move|rotate|scale|offset|translate|shift|nudge|duplicate|copy|clone)/.test(lower)) {
    return "scene_transform";
  }
  return "unknown";
}

export class IntentLayer {
  normalize(input: TaskRequest): NormalizedIntent {
    const prompt = input.prompt.trim();
    const selectionCount = readSelectionCount(input.context);

    return {
      input,
      prompt,
      goalType: detectGoalType(prompt),
      constraints: [
        "Use safe editor actions only.",
        selectionCount > 0 ? "Prefer selected actors when target is not explicit." : "Target can be selection or explicit names."
      ],
      successCriteria: [
        "Generated plan is valid by schema.",
        "Risk and approval flow is respected."
      ]
    };
  }
}
