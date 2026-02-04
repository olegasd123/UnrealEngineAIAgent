import type { PlanAction, PlanOutput } from "../contracts.js";
import type { LlmProvider, PlanInput } from "./types.js";

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

function parseRotateDeltaFromPrompt(prompt: string): { pitch: number; yaw: number; roll: number } | null {
  const lower = prompt.toLowerCase();
  if (!/(rotate|turn)/.test(lower)) {
    return null;
  }

  let pitch = 0;
  let yaw = 0;
  let roll = 0;
  let foundAny = false;

  const patternValueFirst = /([+-]?\d+(?:\.\d+)?)\s*(?:deg|degree|degrees)?\s*(?:on|around)?\s*(yaw|pitch|roll)\b/gi;
  for (const match of lower.matchAll(patternValueFirst)) {
    const value = Number(match[1]);
    const axis = match[2];
    if (!Number.isFinite(value)) {
      continue;
    }
    foundAny = true;
    if (axis === "yaw") yaw += value;
    if (axis === "pitch") pitch += value;
    if (axis === "roll") roll += value;
  }

  const patternAxisFirst = /\b(yaw|pitch|roll)\s*(?:by)?\s*([+-]?\d+(?:\.\d+)?)/gi;
  for (const match of lower.matchAll(patternAxisFirst)) {
    const axis = match[1];
    const value = Number(match[2]);
    if (!Number.isFinite(value)) {
      continue;
    }
    foundAny = true;
    if (axis === "yaw") yaw += value;
    if (axis === "pitch") pitch += value;
    if (axis === "roll") roll += value;
  }

  if (!foundAny) {
    const genericRotate = /\b(?:rotate|turn)\s*([+-]?\d+(?:\.\d+)?)/i.exec(lower);
    if (genericRotate) {
      const value = Number(genericRotate[1]);
      if (Number.isFinite(value)) {
        yaw = value;
        foundAny = true;
      }
    }
  }

  if (!foundAny) {
    return null;
  }

  return { pitch, yaw, roll };
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
    const moveDelta = parseMoveDeltaFromPrompt(input.prompt);
    const rotateDelta = parseRotateDeltaFromPrompt(input.prompt);

    if (moveDelta || rotateDelta) {
      const actions: PlanAction[] = [];
      if (moveDelta) {
        actions.push({
          command: "scene.modifyActor",
          params: {
            target: "selection",
            deltaLocation: moveDelta
          },
          risk: "low"
        });
      }

      if (rotateDelta) {
        actions.push({
          command: "scene.modifyActor",
          params: {
            target: "selection",
            deltaRotation: rotateDelta
          },
          risk: "low"
        });
      }

      return {
        summary: `Planned actor move for prompt: ${input.prompt} (selected: ${selection.length})`,
        steps: [
          "Preview parsed actions",
          "Wait for user approval",
          "Apply approved scene.modifyActor actions with transaction (undo-safe)"
        ],
        actions
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
