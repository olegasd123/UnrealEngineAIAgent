import type { TaskRequest } from "../contracts.js";

export type GoalType =
  | "scene_transform"
  | "scene_create"
  | "scene_delete"
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

function detectGoalType(prompt: string): GoalType {
  const lower = prompt.toLowerCase();
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
    const selection = Array.isArray((input.context as { selection?: unknown[] }).selection)
      ? ((input.context as { selection: unknown[] }).selection as unknown[])
      : [];

    return {
      input,
      prompt,
      goalType: detectGoalType(prompt),
      constraints: [
        "Use safe editor actions only.",
        selection.length > 0 ? "Prefer selected actors when target is not explicit." : "Target can be selection or explicit names."
      ],
      successCriteria: [
        "Generated plan is valid by schema.",
        "Risk and approval flow is respected."
      ]
    };
  }
}
