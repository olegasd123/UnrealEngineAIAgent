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
  const candidates: string[] = [clean];

  const balancedObject = extractFirstBalancedObject(clean);
  if (balancedObject) {
    candidates.push(balancedObject);
  }

  for (const candidate of candidates) {
    const parsed = tryParseWithRepairs(candidate);
    if (parsed !== null) {
      return parsed;
    }
  }

  throw new Error("Could not parse JSON plan from provider response.");
}

export function parsePlanOutput(raw: string): PlanOutput {
  const parsed = parseFirstJsonObject(raw);
  return PlanOutputSchema.parse(parsed);
}

function extractFirstBalancedObject(input: string): string | null {
  let start = -1;
  let depth = 0;
  let inString = false;
  let escaping = false;

  for (let i = 0; i < input.length; i += 1) {
    const char = input[i];

    if (inString) {
      if (escaping) {
        escaping = false;
        continue;
      }
      if (char === "\\") {
        escaping = true;
        continue;
      }
      if (char === "\"") {
        inString = false;
      }
      continue;
    }

    if (char === "\"") {
      inString = true;
      continue;
    }

    if (char === "{") {
      if (depth === 0) {
        start = i;
      }
      depth += 1;
      continue;
    }

    if (char === "}") {
      if (depth === 0) {
        continue;
      }
      depth -= 1;
      if (depth === 0 && start >= 0) {
        return input.slice(start, i + 1);
      }
    }
  }

  return null;
}

function tryParseWithRepairs(input: string): unknown | null {
  const trimmed = input.trim();
  if (!trimmed) {
    return null;
  }

  const attempts = [trimmed, trimmed.replace(/,\s*([}\]])/g, "$1")];
  for (const candidate of attempts) {
    try {
      const parsed = JSON.parse(candidate);
      if (typeof parsed === "string") {
        try {
          return JSON.parse(parsed);
        } catch {
          return null;
        }
      }
      return parsed;
    } catch {
      // Try next parse strategy.
    }
  }

  return null;
}

export function buildPlanPrompt(input: PlanInput): string {
  const payload = {
    prompt: input.prompt,
    mode: input.mode,
    context: input.context
  };

  return [
    "You are a planner for Unreal Editor actions.",
    "Return ONLY one valid JSON object. No markdown. No comments. No extra text.",
    "Allowed actions in this version:",
    "- scene.modifyActor",
    "- scene.createActor",
    "- scene.deleteActor",
    "Use Unreal axis defaults: X forward, Y right, Z up.",
    "If the prompt has both create and transform intent, include both actions in correct order.",
    "If intent is unclear, return actions: [] and explain uncertainty in steps.",
    "JSON schema shape:",
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
          },
          {
            command: "scene.createActor",
            params: {
              actorClass: "StaticMeshActor",
              location: { x: 0, y: 0, z: 0 },
              rotation: { pitch: 0, yaw: 0, roll: 0 },
              count: 1
            },
            risk: "low"
          },
          {
            command: "scene.deleteActor",
            params: {
              target: "selection"
            },
            risk: "high"
          }
        ]
      },
      null,
      2
    ),
    "Rules:",
    "- Keep summary short and concrete.",
    "- steps must be short, ordered, and actionable.",
    "- actions can be empty [] if no executable command is found.",
    "- scene.modifyActor: target must be 'selection'; include deltaLocation and/or deltaRotation.",
    "- scene.createActor: include actorClass; location/rotation optional; count must be integer >= 1.",
    "- scene.deleteActor: target must be 'selection'.",
    "- risk must be low|medium|high.",
    "- Use low for small transform/create, medium for large create (many actors), high for delete.",
    "- Never invent non-existing commands or extra fields.",
    "Examples:",
    JSON.stringify(
      {
        prompt: "create 5 cubes at x 200 y 0 z 100 and rotate yaw 30",
        output: {
          summary: "Create 5 cubes and set spawn transform.",
          steps: ["Preview planned create action", "Wait for user approval", "Execute create action"],
          actions: [
            {
              command: "scene.createActor",
              params: {
                actorClass: "StaticMeshActor",
                location: { x: 200, y: 0, z: 100 },
                rotation: { pitch: 0, yaw: 30, roll: 0 },
                count: 5
              },
              risk: "low"
            }
          ]
        }
      },
      null,
      2
    ),
    JSON.stringify(
      {
        prompt: "delete selected actors",
        output: {
          summary: "Delete current selection.",
          steps: ["Preview delete impact", "Require explicit approval", "Delete selected actors in transaction"],
          actions: [{ command: "scene.deleteActor", params: { target: "selection" }, risk: "high" }]
        }
      },
      null,
      2
    ),
    "Input:",
    JSON.stringify(payload)
  ].join("\n");
}
