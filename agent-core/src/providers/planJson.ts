import { PlanOutputSchema, type PlanOutput } from "../contracts.js";
import type { PlanInput } from "./types.js";

function stripCodeFence(raw: string): string {
  const trimmed = raw.trim();
  if (!trimmed.startsWith("```")) {
    return trimmed;
  }

  const lines = trimmed.split("\n");
  if (lines.length < 3) {
    return trimmed;
  }

  if (!lines[lines.length - 1].trim().startsWith("```")) {
    return trimmed;
  }

  return lines.slice(1, -1).join("\n").trim();
}

function parseFirstJsonObject(raw: string): unknown {
  const clean = stripCodeFence(raw);
  try {
    return JSON.parse(clean);
  } catch {
    const start = clean.indexOf("{");
    const end = clean.lastIndexOf("}");
    if (start >= 0 && end > start) {
      return JSON.parse(clean.slice(start, end + 1));
    }
    throw new Error("Could not parse JSON plan from provider response.");
  }
}

export function parsePlanOutput(raw: string): PlanOutput {
  const parsed = parseFirstJsonObject(raw);
  return PlanOutputSchema.parse(parsed);
}

export function buildPlanPrompt(input: PlanInput): string {
  const payload = {
    prompt: input.prompt,
    mode: input.mode,
    context: input.context
  };

  return [
    "Create a task plan for Unreal Editor actions.",
    "Return ONLY JSON. No markdown, no explanation.",
    "The JSON must follow this exact shape:",
    JSON.stringify(
      {
        summary: "short text",
        steps: ["step 1", "step 2"],
        actions: [
          {
            command: "scene.modifyActor",
            params: {
              target: "selection",
              deltaLocation: { x: 0, y: 0, z: 0 },
              deltaRotation: { pitch: 0, yaw: 0, roll: 0 }
            },
            risk: "low"
          }
        ]
      },
      null,
      2
    ),
    "Rules:",
    "- actions can be empty [] if nothing executable.",
    "- action.command must be scene.modifyActor.",
    "- action.params.target must be selection.",
    "- include deltaLocation and/or deltaRotation.",
    "- risk must be low, medium, or high.",
    "Input:",
    JSON.stringify(payload)
  ].join("\n");
}
