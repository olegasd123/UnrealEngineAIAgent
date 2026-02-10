import { PlanOutputSchema, type PlanOutput, AllowedCommands } from "../contracts.js";
import type { PlanInput } from "../providers/types.js";

const PlanningAllowedCommands = AllowedCommands.filter((command) => !command.startsWith("session."));

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
    prompt: input.request.prompt,
    mode: input.request.mode,
    context: input.request.context,
    normalizedIntent: {
      prompt: input.normalizedPrompt,
      goalType: input.goalType,
      constraints: input.constraints,
      successCriteria: input.successCriteria
    },
    worldState: input.worldState
  };

  return [
    "You are a planner for Unreal Editor actions.",
    "Return ONLY one valid JSON object. No markdown. No comments. No extra text.",
    "Use normalizedIntent as the main control input for planning decisions.",
    "Use worldState to ground decisions in current scene state (selection, environment, lighting, materials, performance, assets).",
    "Prioritize normalizedIntent.constraints over other heuristics. Do not produce actions that violate constraints.",
    "Before finalizing actions, verify the plan against normalizedIntent.successCriteria and update steps/actions to satisfy them.",
    "If constraints and successCriteria conflict or are not satisfiable from context, return actions: [] and explain the blocker in steps.",
    "Allowed actions in this version:",
    ...PlanningAllowedCommands.map((command) => `- ${command}`),
    "Use Unreal axis defaults: X forward, Y right, Z up.",
    "If the prompt has both create and transform intent, include both actions in correct order.",
    "If intent is unclear, return actions: [] and explain uncertainty in steps.",
    "JSON schema shape:",
    JSON.stringify(
      {
        summary: "short text",
        steps: ["step 1", "step 2"],
        goal: {
          id: "goal_lighting_tune",
          description: "Tune lighting for the requested style.",
          priority: "medium"
        },
        subgoals: [
          {
            id: "sg_validate_scope",
            description: "Validate scope and targets.",
            dependsOn: []
          },
          {
            id: "sg_prepare_actions",
            description: "Prepare ordered Unreal actions.",
            dependsOn: ["sg_validate_scope"]
          }
        ],
        checks: [
          {
            id: "check_constraint_1",
            description: "Use safe editor actions only.",
            type: "constraint",
            source: "intent.constraints",
            status: "pending",
            onFail: "stop"
          },
          {
            id: "check_success_1",
            description: "Generated plan is valid by schema.",
            type: "success",
            source: "intent.successCriteria",
            status: "pending",
            onFail: "revise_subgoals"
          }
        ],
        stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }],
        actions: [
          {
            command: "scene.modifyActor",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              deltaLocation: { x: 0, y: 0, z: 0 },
              deltaRotation: { pitch: 0, yaw: 0, roll: 0 },
              deltaScale: { x: 0, y: 0, z: 0 },
              scale: { x: 1, y: 1, z: 1 }
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
              target: "selection",
              actorNames: ["actor_name_if_target_byName"]
            },
            risk: "high"
          },
          {
            command: "scene.modifyComponent",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              componentName: "StaticMeshComponent0",
              deltaLocation: { x: 0, y: 0, z: 0 },
              deltaRotation: { pitch: 0, yaw: 0, roll: 0 },
              deltaScale: { x: 0, y: 0, z: 0 },
              scale: { x: 1, y: 1, z: 1 },
              visibility: true
            },
            risk: "low"
          },
          {
            command: "scene.setComponentMaterial",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              componentName: "StaticMeshComponent0",
              materialPath: "/Game/Materials/M_Wall.M_Wall",
              materialSlot: 0
            },
            risk: "low"
          },
          {
            command: "scene.setComponentStaticMesh",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              componentName: "StaticMeshComponent0",
              meshPath: "/Game/Props/SM_Crate.SM_Crate"
            },
            risk: "low"
          },
          {
            command: "scene.addActorTag",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              tag: "MyTag"
            },
            risk: "low"
          },
          {
            command: "scene.setActorFolder",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              folderPath: "Props/SetA"
            },
            risk: "low"
          },
          {
            command: "scene.addActorLabelPrefix",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              prefix: "SetA_"
            },
            risk: "low"
          },
          {
            command: "scene.duplicateActors",
            params: {
              target: "selection",
              actorNames: ["actor_name_if_target_byName"],
              count: 2,
              offset: { x: 100, y: 0, z: 0 }
            },
            risk: "medium"
          }
        ]
      },
      null,
      2
    ),
    "Rules:",
    "- Keep summary short and concrete.",
    "- steps must be short, ordered, and actionable.",
    "- goal is required with id, description, priority(low|medium|high).",
    "- subgoals is an ordered list with unique id and valid dependsOn references.",
    "- checks should map to normalizedIntent.constraints and normalizedIntent.successCriteria when available.",
    "- stopConditions must include at least: all_checks_passed, max_iterations, user_denied.",
    "- actions can be empty [] if no executable command is found.",
    "- scene.modifyActor: target must be 'selection' or 'byName'; include actorNames when using 'byName'; include deltaLocation and/or deltaRotation and/or deltaScale and/or scale.",
    "- scene.createActor: include actorClass; location/rotation optional; count must be integer >= 1.",
    "- scene.deleteActor: target must be 'selection' or 'byName'; include actorNames when using 'byName'.",
    "- scene.modifyComponent: target must be 'selection' or 'byName'; include actorNames when using 'byName'; include componentName; include a transform or visibility.",
    "- scene.setComponentMaterial: include componentName + materialPath; optional materialSlot.",
    "- scene.setComponentStaticMesh: include componentName + meshPath.",
    "- scene.addActorTag: target must be 'selection' or 'byName'; include actorNames when using 'byName'; include tag.",
    "- scene.setActorFolder: include folderPath (can be empty to clear).",
    "- scene.addActorLabelPrefix: include prefix.",
    "- scene.duplicateActors: include count (1-20). Optional offset.",
    "- session transaction begin/commit/rollback are internal. Do not include any session.* action.",
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
          goal: {
            id: "goal_scene_create",
            description: "Create requested actors with requested transform.",
            priority: "medium"
          },
          subgoals: [
            { id: "sg_validate_scope", description: "Validate create scope.", dependsOn: [] },
            { id: "sg_prepare_actions", description: "Prepare create actions.", dependsOn: ["sg_validate_scope"] },
            { id: "sg_execute_actions", description: "Execute approved actions.", dependsOn: ["sg_prepare_actions"] }
          ],
          checks: [
            {
              id: "check_constraint_1",
              description: "Use safe editor actions only.",
              type: "constraint",
              source: "intent.constraints",
              status: "pending",
              onFail: "stop"
            }
          ],
          stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }],
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
          steps: ["Preview delete impact", "Require explicit approval", "Delete selected actors (internal transaction)"],
          goal: { id: "goal_scene_delete", description: "Delete selected actors.", priority: "high" },
          subgoals: [
            { id: "sg_validate_scope", description: "Validate delete scope.", dependsOn: [] },
            { id: "sg_execute_actions", description: "Execute approved delete.", dependsOn: ["sg_validate_scope"] }
          ],
          checks: [
            {
              id: "check_safety_high_risk_approval",
              description: "High-risk actions require explicit user approval.",
              type: "safety",
              source: "planner",
              status: "pending",
              onFail: "require_approval"
            }
          ],
          stopConditions: [
            { type: "all_checks_passed" },
            { type: "max_iterations", value: 1 },
            { type: "user_denied" },
            { type: "risk_threshold", maxRisk: "medium" }
          ],
          actions: [{ command: "scene.deleteActor", params: { target: "selection" }, risk: "high" }]
        }
      },
      null,
      2
    ),
    JSON.stringify(
      {
        prompt: "move actor3 +250 on X",
        output: {
          summary: "Move actor3 along X.",
          steps: ["Preview planned modify action", "Wait for user approval", "Apply modify action"],
          goal: { id: "goal_scene_transform", description: "Move actor3 along X.", priority: "low" },
          subgoals: [
            { id: "sg_validate_scope", description: "Validate transform target.", dependsOn: [] },
            { id: "sg_execute_actions", description: "Execute approved transform action.", dependsOn: ["sg_validate_scope"] }
          ],
          checks: [],
          stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }],
          actions: [
            {
              command: "scene.modifyActor",
              params: { target: "byName", actorNames: ["actor3"], deltaLocation: { x: 250, y: 0, z: 0 } },
              risk: "low"
            }
          ]
        }
      },
      null,
      2
    ),
    "Input:",
    JSON.stringify(payload)
  ].join("\n");
}
