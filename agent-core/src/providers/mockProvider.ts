import type { LlmProvider, PlanInput, PlanOutput } from "./types.js";

function parseMoveDeltaFromPrompt(prompt: string): { x: number; y: number; z: number } | null {
  const lower = prompt.toLowerCase();
  if (!/(move|offset|translate)/.test(lower)) {
    return null;
  }

  let x = 0;
  let y = 0;
  let z = 0;
  let foundAny = false;

  const patternValueFirst = /([+-]?\d+(?:\.\d+)?)\s*(?:units?\s*)?(?:on|along|in)?\s*(x|y|z)\b/gi;
  for (const match of lower.matchAll(patternValueFirst)) {
    const value = Number(match[1]);
    const axis = match[2];
    if (!Number.isFinite(value)) {
      continue;
    }
    foundAny = true;
    if (axis === "x") x += value;
    if (axis === "y") y += value;
    if (axis === "z") z += value;
  }

  const patternAxisFirst = /\b(x|y|z)\s*(?:axis)?\s*(?:by)?\s*([+-]?\d+(?:\.\d+)?)/gi;
  for (const match of lower.matchAll(patternAxisFirst)) {
    const axis = match[1];
    const value = Number(match[2]);
    if (!Number.isFinite(value)) {
      continue;
    }
    foundAny = true;
    if (axis === "x") x += value;
    if (axis === "y") y += value;
    if (axis === "z") z += value;
  }

  if (!foundAny) {
    return null;
  }

  return { x, y, z };
}

export class MockProvider implements LlmProvider {
  public readonly name: "openai" | "gemini";

  constructor(name: "openai" | "gemini") {
    this.name = name;
  }

  async planTask(input: PlanInput): Promise<PlanOutput> {
    const selection = Array.isArray((input.context as { selection?: unknown }).selection)
      ? ((input.context as { selection: unknown[] }).selection as unknown[])
      : [];
    const delta = parseMoveDeltaFromPrompt(input.prompt);

    if (delta) {
      return {
        summary: `Planned actor move for prompt: ${input.prompt} (selected: ${selection.length})`,
        steps: [
          "Preview parsed movement action",
          "Wait for user approval",
          "Apply scene.modifyActor with transaction (undo-safe)"
        ],
        actions: [
          {
            command: "scene.modifyActor",
            params: {
              target: "selection",
              deltaLocation: delta
            },
            risk: "low"
          }
        ]
      };
    }

    return {
      summary: `Draft plan for: ${input.prompt} (selected: ${selection.length})`,
      steps: [
        "Collect scene context",
        "Build action list",
        "No executable action parsed from prompt yet"
      ],
      actions: []
    };
  }
}
