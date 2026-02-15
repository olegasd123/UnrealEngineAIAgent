import type { PlanAction, PlanOutput, TaskRequest } from "../contracts.js";
import type { GoalType } from "../intent/intentLayer.js";

interface FallbackPlanMetadata {
  goalType?: GoalType;
  constraints?: string[];
  successCriteria?: string[];
}

function uniqueStrings(values: string[]): string[] {
  return Array.from(new Set(values.map((value) => value.trim()).filter((value) => value.length > 0)));
}

function readSelectionNamesFromContext(context: TaskRequest["context"]): string[] {
  const names: string[] = [];
  if (Array.isArray(context.selectionNames)) {
    for (const item of context.selectionNames) {
      if (typeof item === "string") {
        names.push(item);
      }
    }
  }

  if (Array.isArray(context.selection)) {
    for (const item of context.selection) {
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

function isReferentialPrompt(prompt: string): boolean {
  return /\b(it|them|that|those|selected|selection|same|previous)\b/i.test(prompt);
}

function parseAxisValues(
  source: string,
  axes: readonly string[]
): Partial<Record<string, number>> {
  const values: Partial<Record<string, number>> = {};
  for (const axis of axes) {
    const pattern = new RegExp(
      `\\b${axis}\\b\\s*(?:axis\\b)?\\s*(?:=|:|by)?\\s*([+-]?\\d+(?:\\.\\d+)?)`,
      "i"
    );
    const match = pattern.exec(source);
    if (!match) {
      continue;
    }
    const value = Number(match[1]);
    if (!Number.isFinite(value)) {
      continue;
    }
    values[axis] = value;
  }
  return values;
}

function parseMoveDeltaFromPrompt(prompt: string): { x: number; y: number; z: number } | null {
  const lower = prompt.toLowerCase();
  if (!/(move|offset|translate|shift|nudge|push|pull)/.test(lower)) {
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

  const directionalPattern = /\b(up|down|left|right|forward|back|backward)\s*(?:to|by)?\s*([+-]?\d+(?:\.\d+)?)/gi;
  for (const match of lower.matchAll(directionalPattern)) {
    const direction = match[1];
    const value = Number(match[2]);
    if (!Number.isFinite(value)) {
      continue;
    }

    foundAny = true;
    if (direction === "up") z += value;
    if (direction === "down") z -= value;
    if (direction === "right") y += value;
    if (direction === "left") y -= value;
    if (direction === "forward") x += value;
    if (direction === "back" || direction === "backward") x -= value;
  }

  if (!foundAny) {
    return null;
  }

  return { x, y, z };
}

function parseRotateDeltaFromPrompt(prompt: string): { pitch: number; yaw: number; roll: number } | null {
  const lower = prompt.toLowerCase();
  if (!/(rotate|turn|spin|orient)/.test(lower)) {
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

  const zAsYawPattern = /\b([+-]?\d+(?:\.\d+)?)\s*(?:deg|degree|degrees)?\s*(?:around|on)?\s*z\b/gi;
  for (const match of lower.matchAll(zAsYawPattern)) {
    const value = Number(match[1]);
    if (!Number.isFinite(value)) {
      continue;
    }
    yaw += value;
    foundAny = true;
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

function parseScaleFromPrompt(prompt: string): {
  delta?: { x: number; y: number; z: number };
  absolute?: { x: number; y: number; z: number };
} | null {
  const lower = prompt.toLowerCase();
  if (!/(scale|resize|grow|shrink)/.test(lower)) {
    return null;
  }

  let deltaX = 0;
  let deltaY = 0;
  let deltaZ = 0;
  let hasDelta = false;
  let absX: number | undefined;
  let absY: number | undefined;
  let absZ: number | undefined;
  let hasAbsolute = false;

  const patternAxisBy = /\b(x|y|z)\s*(?:scale)?\s*by\s*([+-]?\d+(?:\.\d+)?)/gi;
  for (const match of lower.matchAll(patternAxisBy)) {
    const axis = match[1];
    const value = Number(match[2]);
    if (!Number.isFinite(value)) {
      continue;
    }
    hasDelta = true;
    if (axis === "x") deltaX += value;
    if (axis === "y") deltaY += value;
    if (axis === "z") deltaZ += value;
  }

  const patternAxisTo = /\b(x|y|z)\s*(?:scale)?\s*(?:to|=)\s*([+-]?\d+(?:\.\d+)?)/gi;
  for (const match of lower.matchAll(patternAxisTo)) {
    const axis = match[1];
    const value = Number(match[2]);
    if (!Number.isFinite(value)) {
      continue;
    }
    hasAbsolute = true;
    if (axis === "x") absX = value;
    if (axis === "y") absY = value;
    if (axis === "z") absZ = value;
  }

  const patternAxisFirst = /\b(x|y|z)\s*(?:scale)?\s*([+-]?\d+(?:\.\d+)?)/gi;
  for (const match of lower.matchAll(patternAxisFirst)) {
    const axis = match[1];
    const value = Number(match[2]);
    if (!Number.isFinite(value)) {
      continue;
    }
    if (axis === "x") deltaX += value;
    if (axis === "y") deltaY += value;
    if (axis === "z") deltaZ += value;
    hasDelta = true;
  }

  const uniformBy = /\bscale\s*by\s*([+-]?\d+(?:\.\d+)?)/i.exec(lower);
  if (uniformBy) {
    const value = Number(uniformBy[1]);
    if (Number.isFinite(value)) {
      deltaX += value;
      deltaY += value;
      deltaZ += value;
      hasDelta = true;
    }
  }

  const uniformTo = /\b(?:set\s+)?scale\s*(?:to|=)\s*([+-]?\d+(?:\.\d+)?)/i.exec(lower);
  if (uniformTo) {
    const value = Number(uniformTo[1]);
    if (Number.isFinite(value)) {
      absX = value;
      absY = value;
      absZ = value;
      hasAbsolute = true;
    }
  }

  if (!hasDelta && !hasAbsolute) {
    return null;
  }

  const absolute = hasAbsolute
    ? {
        x: absX ?? 1,
        y: absY ?? 1,
        z: absZ ?? 1
      }
    : undefined;

  const delta = hasDelta ? { x: deltaX, y: deltaY, z: deltaZ } : undefined;
  return { delta, absolute };
}

function parseCreateLocationFromPrompt(prompt: string): { x: number; y: number; z: number } | undefined {
  const lower = prompt.toLowerCase();
  const hasLocationHint = /\b(at|to|location|position|pos)\b/.test(lower);
  const axisValues = parseAxisValues(lower, ["x", "y", "z"]);
  const foundAxes = ["x", "y", "z"].filter((axis) => typeof axisValues[axis] === "number");
  if (foundAxes.length === 0) {
    return undefined;
  }

  if (!hasLocationHint && foundAxes.length < 2) {
    return undefined;
  }

  return {
    x: axisValues.x ?? 0,
    y: axisValues.y ?? 0,
    z: axisValues.z ?? 0
  };
}

function parseCreateRotationFromPrompt(
  prompt: string
): { pitch: number; yaw: number; roll: number } | undefined {
  const lower = prompt.toLowerCase();
  if (!/(rotate|rotation|orient|yaw|pitch|roll)/.test(lower)) {
    return undefined;
  }

  const values = parseAxisValues(lower, ["pitch", "yaw", "roll"]);
  const foundAny = ["pitch", "yaw", "roll"].some((axis) => typeof values[axis] === "number");
  if (!foundAny) {
    return undefined;
  }

  return {
    pitch: values.pitch ?? 0,
    yaw: values.yaw ?? 0,
    roll: values.roll ?? 0
  };
}

function parseCreateActorFromPrompt(prompt: string): {
  actorClass: string;
  location?: { x: number; y: number; z: number };
  rotation?: { pitch: number; yaw: number; roll: number };
  count: number;
} | null {
  const lower = prompt.toLowerCase();
  if (!/(create|spawn|add)/.test(lower) || !/(actor|actors|object|mesh|cube|sphere|light|camera)/.test(lower)) {
    return null;
  }

  let actorClass = "Actor";
  if (/(mesh|cube|static mesh)/.test(lower)) {
    actorClass = "StaticMeshActor";
  } else if (/(sphere)/.test(lower)) {
    actorClass = "StaticMeshActor";
  } else if (/(light|point light)/.test(lower)) {
    actorClass = "PointLight";
  } else if (/(directional light|sun light)/.test(lower)) {
    actorClass = "DirectionalLight";
  } else if (/(spot light)/.test(lower)) {
    actorClass = "SpotLight";
  } else if (/(camera)/.test(lower)) {
    actorClass = "CameraActor";
  }

  const countMatch =
    /\b(?:create|spawn|add)\s+(\d+)\b/i.exec(prompt) ??
    /\b(\d+)\s*x\s*(?:actors?|objects?|cubes?|spheres?|lights?)\b/i.exec(prompt) ??
    /\b(\d+)\s+(?:actors?|objects?|cubes?|spheres?|lights?)\b/i.exec(prompt);
  const parsedCount = countMatch ? Number(countMatch[1]) : 1;
  const count = Number.isFinite(parsedCount) ? Math.max(1, Math.min(100, Math.trunc(parsedCount))) : 1;

  const location = parseCreateLocationFromPrompt(prompt);
  const rotation = parseCreateRotationFromPrompt(prompt) ?? parseRotateDeltaFromPrompt(prompt) ?? undefined;

  return {
    actorClass,
    location,
    rotation,
    count
  };
}

function shouldDeleteSelection(prompt: string): boolean {
  const lower = prompt.toLowerCase();
  return /(delete|remove|destroy|erase)/.test(lower) && /(selected|selection|actor|actors)/.test(lower);
}

function parseActorNamesFromPrompt(prompt: string): string[] | null {
  const names = new Set<string>();
  const lower = prompt.toLowerCase();

  const directActorToken = /\bactor[0-9a-z_]+\b/gi;
  for (const match of prompt.matchAll(directActorToken)) {
    const value = match[0];
    if (value && !/^actors?$/i.test(value)) {
      names.add(value);
    }
  }

  const listPattern = /\bactors?\s+([0-9a-z_,\s]+)/gi;
  for (const match of prompt.matchAll(listPattern)) {
    let chunk = match[1] ?? "";
    const stopWord = /\b(on|along|around|by|to|at|with|and|rotate|move|offset|translate|shift|nudge|push|pull|delete|remove|destroy|erase)\b/i;
    const stopMatch = stopWord.exec(chunk);
    if (stopMatch) {
      chunk = chunk.slice(0, stopMatch.index);
    }
    for (const token of chunk.split(/[\s,]+/)) {
      const trimmed = token.trim();
      if (!trimmed) {
        continue;
      }
      if (/^actors?$/i.test(trimmed)) {
        continue;
      }
      names.add(trimmed);
    }
  }

  const namedPattern = /\b(?:named|name)\s+([0-9a-z_]+)/gi;
  for (const match of prompt.matchAll(namedPattern)) {
    const value = match[1];
    if (value && !/^actors?$/i.test(value)) {
      names.add(value);
    }
  }

  if (names.size === 0) {
    if (/(delete|remove|destroy|erase)/.test(lower)) {
      return null;
    }
    return null;
  }

  return Array.from(names);
}

function parseComponentVisibilityFromPrompt(prompt: string): {
  componentName: string;
  visibility: boolean;
} | null {
  const lower = prompt.toLowerCase();
  if (!/(component|comp)/.test(lower) || !/(hide|show|visible|visibility)/.test(lower)) {
    return null;
  }

  const componentMatch =
    /\bcomponent\s+([0-9a-z_]+)/i.exec(prompt) ??
    /\bcomp\s+([0-9a-z_]+)/i.exec(prompt);
  if (!componentMatch) {
    return null;
  }

  const componentName = componentMatch[1];
  if (!componentName) {
    return null;
  }

  const visibility = /(show|visible|visibility on)/i.test(prompt);
  const hidden = /(hide|hidden|visibility off)/i.test(prompt);
  if (!visibility && !hidden) {
    return null;
  }

  return {
    componentName,
    visibility: visibility && !hidden
  };
}

function parseComponentNameFromPrompt(prompt: string): string | null {
  const quoted =
    /\bcomponent\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\bcomponent\s+\'([^\']+)\'/i.exec(prompt) ??
    /\bcomponent\s*:\s*([0-9a-z_]+)/i.exec(prompt);
  if (quoted && quoted[1]) {
    return quoted[1];
  }

  const plain = /\bcomponent\s+([0-9a-z_]+)/i.exec(prompt) ?? /\bcomp\s+([0-9a-z_]+)/i.exec(prompt);
  if (plain && plain[1]) {
    return plain[1];
  }

  const lower = prompt.toLowerCase();
  if (/(static\s*mesh\s*component|mesh\s*component)/.test(lower)) {
    return "StaticMeshComponent0";
  }
  if (/(root\s*component)/.test(lower)) {
    return "RootComponent";
  }
  if (/(camera\s*component)/.test(lower)) {
    return "CameraComponent";
  }
  if (/(light\s*component)/.test(lower)) {
    return "LightComponent";
  }

  return null;
}

function parseAssetPathFromPrompt(prompt: string): string | null {
  const objectPath =
    /(?:Material|StaticMesh)?\'(\/(?:Game|Engine)\/[0-9A-Za-z_\/\.\-]+)\'/i.exec(prompt) ??
    /(\/(?:Game|Engine)\/[0-9A-Za-z_\/\.\-]+)/i.exec(prompt);
  if (objectPath) {
    return objectPath[1] ?? objectPath[0];
  }

  const quotedFull = /\"(\/(?:Game|Engine)\/[0-9A-Za-z_\/\.\-]+)\"/i.exec(prompt);
  if (quotedFull && quotedFull[1]) {
    return quotedFull[1];
  }

  const pathMatch = /(?:^|\s)([0-9A-Za-z_]+\/[0-9A-Za-z_\/\.\-]+)/.exec(prompt);
  if (pathMatch) {
    return pathMatch[1];
  }

  const quoted = /\"([0-9A-Za-z_\/\.\-]+)\"/.exec(prompt);
  if (quoted && quoted[1] && /[\/\.]/.test(quoted[1])) {
    return quoted[1];
  }

  return null;
}

function parseMaterialAssignmentFromPrompt(prompt: string): {
  componentName: string;
  materialPath: string;
  materialSlot?: number;
} | null {
  if (!/(material|shader)/i.test(prompt) || !/(set|assign|apply|replace)/i.test(prompt)) {
    return null;
  }

  const componentName = parseComponentNameFromPrompt(prompt);
  const materialPath = parseAssetPathFromPrompt(prompt);
  if (!componentName || !materialPath) {
    return null;
  }

  const slotMatch = /\bslot\s*(\d+)\b/i.exec(prompt);
  const materialSlot = slotMatch ? Number(slotMatch[1]) : undefined;
  return {
    componentName,
    materialPath,
    materialSlot: Number.isFinite(materialSlot ?? NaN) ? materialSlot : undefined
  };
}

function parseStaticMeshAssignmentFromPrompt(prompt: string): {
  componentName: string;
  meshPath: string;
} | null {
  if (!/(static\s*mesh|mesh)/i.test(prompt) || !/(set|assign|replace|swap)/i.test(prompt)) {
    return null;
  }

  const componentName = parseComponentNameFromPrompt(prompt);
  const meshPath = parseAssetPathFromPrompt(prompt);
  if (!componentName || !meshPath) {
    return null;
  }

  return { componentName, meshPath };
}

function parseActorTagFromPrompt(prompt: string): string | null {
  const lower = prompt.toLowerCase();
  if (!/(tag|label)/.test(lower)) {
    return null;
  }

  const tagMatch =
    /\btag\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\btag\s+([0-9a-z_]+)/i.exec(prompt);
  if (!tagMatch) {
    return null;
  }

  const tag = tagMatch[1]?.trim();
  if (!tag) {
    return null;
  }

  return tag;
}

function parseActorFolderFromPrompt(prompt: string): string | null {
  if (!/(folder|group)/i.test(prompt) || !/(set|move|place|put)/i.test(prompt)) {
    return null;
  }

  const match =
    /\bfolder\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\bfolder\s+\'([^\']+)\'/i.exec(prompt) ??
    /\bfolder\s+([0-9A-Za-z_\/\-]+)/i.exec(prompt);
  if (match && match[1]) {
    return match[1];
  }

  return null;
}

function parseActorLabelPrefixFromPrompt(prompt: string): string | null {
  if (!/(label|name)/i.test(prompt) || !/(prefix|add prefix)/i.test(prompt)) {
    return null;
  }

  const match =
    /\bprefix\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\bprefix\s+\'([^\']+)\'/i.exec(prompt) ??
    /\bprefix\s+([0-9A-Za-z_]+)/i.exec(prompt);
  if (match && match[1]) {
    return match[1];
  }

  return null;
}

function parseDuplicateFromPrompt(prompt: string): { count: number; offset?: { x: number; y: number; z: number } } | null {
  if (!/(duplicate|copy|clone)/i.test(prompt)) {
    return null;
  }

  const countMatch =
    /\bduplicate\s+(\d+)\b/i.exec(prompt) ??
    /\bcopy\s+(\d+)\b/i.exec(prompt) ??
    /\b(\d+)\s+copies?\b/i.exec(prompt);
  const parsedCount = countMatch ? Number(countMatch[1]) : 1;
  const count = Number.isFinite(parsedCount) ? Math.max(1, Math.min(20, Math.trunc(parsedCount))) : 1;
  const offset = parseMoveDeltaFromPrompt(prompt) ?? undefined;
  return { count, offset };
}

function hasWriteIntent(prompt: string): boolean {
  return /(move|offset|translate|shift|rotate|turn|spin|scale|resize|grow|shrink|create|spawn|add|delete|remove|destroy|erase|set|assign|apply|replace|duplicate|copy|clone|paint|sculpt)/i.test(
    prompt
  );
}

function wantsSelectionContext(prompt: string): boolean {
  if (!/\b(selected|selection)\b/i.test(prompt)) {
    return false;
  }

  const readHint =
    /\b(what|which|show|list|describe|summarize|summary|details?|info|context|get|read|inspect)\b/i.test(prompt) ||
    /\?/.test(prompt);
  return readHint;
}

function wantsSceneSummary(prompt: string): boolean {
  const sceneHint =
    /\b(scene|level|map|world)\b/i.test(prompt) ||
    /\bhow many\s+actors?\b/i.test(prompt) ||
    /\bactor\s+count\b/i.test(prompt);
  if (!sceneHint) {
    return false;
  }

  return /\b(summary|overview|status|context|info|details?|count|list|show|describe|get|read|inspect|what)\b/i.test(prompt);
}

function buildContextActionsFromPrompt(prompt: string): PlanAction[] {
  if (hasWriteIntent(prompt)) {
    return [];
  }

  const actions: PlanAction[] = [];
  if (wantsSceneSummary(prompt) || /\bfetch more context\b/i.test(prompt)) {
    actions.push({
      command: "context.getSceneSummary",
      params: {},
      risk: "low"
    });
  }

  if (wantsSelectionContext(prompt)) {
    actions.push({
      command: "context.getSelection",
      params: {},
      risk: "low"
    });
  }

  return actions;
}

function normalizeIdPart(value: string): string {
  const normalized = value.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "");
  return normalized.length > 0 ? normalized : "item";
}

function buildGoal(goalType: GoalType | undefined, prompt: string): PlanOutput["goal"] {
  if (goalType && goalType !== "unknown") {
    return {
      id: `goal_${goalType}`,
      description: `Complete task for goal type: ${goalType}.`,
      priority: goalType === "scene_delete" ? "high" : "medium"
    };
  }

  return {
    id: `goal_prompt_${normalizeIdPart(prompt).slice(0, 40)}`,
    description: "Complete the requested Unreal Editor task.",
    priority: "medium"
  };
}

function buildSubgoals(hasActions: boolean, hasWriteActions: boolean): PlanOutput["subgoals"] {
  if (!hasActions) {
    return [
      {
        id: "sg_collect_context",
        description: "Collect more context to build executable actions.",
        dependsOn: []
      }
    ];
  }

  if (!hasWriteActions) {
    return [
      {
        id: "sg_collect_context",
        description: "Collect requested scene context from Unreal Editor.",
        dependsOn: []
      },
      {
        id: "sg_return_context",
        description: "Return collected context to user.",
        dependsOn: ["sg_collect_context"]
      }
    ];
  }

  return [
    {
      id: "sg_validate_scope",
      description: "Validate task scope and target actors/components.",
      dependsOn: []
    },
    {
      id: "sg_prepare_actions",
      description: "Prepare ordered Unreal Editor actions.",
      dependsOn: ["sg_validate_scope"]
    },
    {
      id: "sg_execute_actions",
      description: "Execute approved actions safely in transaction.",
      dependsOn: ["sg_prepare_actions"]
    }
  ];
}

function buildChecks(
  metadata: FallbackPlanMetadata,
  hasHighRiskAction: boolean
): PlanOutput["checks"] {
  const checks: PlanOutput["checks"] = [];
  for (const [index, text] of (metadata.constraints ?? []).entries()) {
    checks.push({
      id: `check_constraint_${index + 1}`,
      description: text,
      type: "constraint",
      source: "intent.constraints",
      status: "pending",
      onFail: "stop"
    });
  }

  for (const [index, text] of (metadata.successCriteria ?? []).entries()) {
    checks.push({
      id: `check_success_${index + 1}`,
      description: text,
      type: "success",
      source: "intent.successCriteria",
      status: "pending",
      onFail: "revise_subgoals"
    });
  }

  if (hasHighRiskAction) {
    checks.push({
      id: "check_safety_high_risk_approval",
      description: "High-risk actions require explicit user approval before execution.",
      type: "safety",
      source: "planner",
      status: "pending",
      onFail: "require_approval"
    });
  }

  return checks;
}

function buildStopConditions(hasHighRiskAction: boolean): PlanOutput["stopConditions"] {
  const conditions: PlanOutput["stopConditions"] = [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }];
  if (hasHighRiskAction) {
    conditions.push({ type: "risk_threshold", maxRisk: "medium" });
  }
  return conditions;
}

function formatNumber(value: number): string {
  if (!Number.isFinite(value)) {
    return "0";
  }
  if (Number.isInteger(value)) {
    return String(value);
  }
  return String(Math.round(value * 1000) / 1000);
}

function describeTargetCount(actorNames: string[] | undefined): string {
  return (actorNames?.length ?? 0) > 1 ? "selected actors" : "selected actor";
}

function describeMoveSummary(action: PlanAction): string | undefined {
  if (action.command !== "scene.modifyActor") {
    return undefined;
  }

  const params = action.params;
  if (params.deltaRotation || params.deltaScale || params.scale || !params.deltaLocation) {
    return undefined;
  }

  const targetText = describeTargetCount(params.actorNames);
  const { x, y, z } = params.deltaLocation;
  const nonZeroAxes = [x, y, z].filter((value) => Math.abs(value) > 0).length;
  if (nonZeroAxes !== 1) {
    return `Move ${targetText}.`;
  }

  if (Math.abs(x) > 0) {
    if (x < 0) {
      return `Move ${targetText} back ${formatNumber(Math.abs(x))} units along X.`;
    }
    return `Move ${targetText} along X by ${formatNumber(x)} units.`;
  }

  if (Math.abs(y) > 0) {
    if (y < 0) {
      return `Move ${targetText} left ${formatNumber(Math.abs(y))} units along Y.`;
    }
    return `Move ${targetText} right ${formatNumber(y)} units along Y.`;
  }

  if (z < 0) {
    return `Move ${targetText} down ${formatNumber(Math.abs(z))} units along Z.`;
  }
  return `Move ${targetText} up ${formatNumber(z)} units along Z.`;
}

function buildActionSummary(input: TaskRequest, actions: PlanAction[]): string {
  const moveSummary = describeMoveSummary(actions[0]!);
  if (moveSummary) {
    return moveSummary;
  }

  const first = actions[0];
  if (!first) {
    return `Draft plan for: ${input.prompt} (selected: ${readSelectionNamesFromContext(input.context).length})`;
  }

  if (first.command === "scene.createActor") {
    const count = first.params.count ?? 1;
    return count === 1
      ? `Create 1 ${first.params.actorClass} actor.`
      : `Create ${formatNumber(count)} ${first.params.actorClass} actors.`;
  }

  if (first.command === "scene.deleteActor") {
    const count = first.params.actorNames?.length ?? 0;
    return count > 1 ? `Delete ${count} selected actors.` : "Delete selected actor.";
  }

  if (first.command === "context.getSceneSummary") {
    return actions.some((action) => action.command === "context.getSelection")
      ? "Collect current scene and selection context."
      : "Collect current scene summary context.";
  }

  if (first.command === "context.getSelection") {
    return "Collect current selection context.";
  }

  if (actions.length === 1) {
    return `Apply 1 scene action for: ${input.prompt}.`;
  }

  return `Apply ${actions.length} scene actions for: ${input.prompt}.`;
}

export function buildRuleBasedPlan(input: TaskRequest, metadata: FallbackPlanMetadata = {}): PlanOutput {
  const selectionNames = readSelectionNamesFromContext(input.context);
  const moveDelta = parseMoveDeltaFromPrompt(input.prompt);
  const rotateDelta = parseRotateDeltaFromPrompt(input.prompt);
  const scale = parseScaleFromPrompt(input.prompt);
  const createActor = parseCreateActorFromPrompt(input.prompt);
  const actorNames = parseActorNamesFromPrompt(input.prompt);
  const referentialPrompt = isReferentialPrompt(input.prompt);
  const resolvedActorNames =
    actorNames && actorNames.length > 0
      ? actorNames
      : referentialPrompt && selectionNames.length > 0
      ? selectionNames
      : null;
  const deleteSelection = shouldDeleteSelection(input.prompt);
  const deleteByName = /(delete|remove|destroy|erase)/.test(input.prompt.toLowerCase()) && Boolean(resolvedActorNames?.length);
  const componentVisibility = parseComponentVisibilityFromPrompt(input.prompt);
  const componentName = parseComponentNameFromPrompt(input.prompt);
  const materialAssignment = parseMaterialAssignmentFromPrompt(input.prompt);
  const meshAssignment = parseStaticMeshAssignmentFromPrompt(input.prompt);
  const tag = parseActorTagFromPrompt(input.prompt);
  const folderPath = parseActorFolderFromPrompt(input.prompt);
  const labelPrefix = parseActorLabelPrefixFromPrompt(input.prompt);
  const duplicate = parseDuplicateFromPrompt(input.prompt);

  const actions: PlanAction[] = [];
  if (createActor) {
    actions.push({
      command: "scene.createActor",
      params: createActor,
      risk: createActor.count > 10 ? "medium" : "low"
    });
  }

  if (moveDelta || rotateDelta || scale?.delta || scale?.absolute) {
    const params: {
      target: "selection" | "byName";
      actorNames?: string[];
      deltaLocation?: { x: number; y: number; z: number };
      deltaRotation?: { pitch: number; yaw: number; roll: number };
      deltaScale?: { x: number; y: number; z: number };
      scale?: { x: number; y: number; z: number };
    } = resolvedActorNames && resolvedActorNames.length > 0
      ? { target: "byName", actorNames: resolvedActorNames }
      : { target: "selection" };
    if (moveDelta) {
      params.deltaLocation = moveDelta;
    }
    if (rotateDelta) {
      params.deltaRotation = rotateDelta;
    }
    if (scale?.delta) {
      params.deltaScale = scale.delta;
    }
    if (scale?.absolute) {
      params.scale = scale.absolute;
    }

    actions.push({
      command: "scene.modifyActor",
      params,
      risk: "low"
    });
  }

  if (deleteByName && resolvedActorNames) {
    actions.push({
      command: "scene.deleteActor",
      params: { target: "byName", actorNames: resolvedActorNames },
      risk: "high"
    });
  }

  if (deleteSelection) {
    actions.push({
      command: "scene.deleteActor",
      params: { target: "selection" },
      risk: "high"
    });
  }

  if (componentVisibility) {
    actions.push({
      command: "scene.modifyComponent",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        componentName: componentVisibility.componentName,
        visibility: componentVisibility.visibility
      },
      risk: "low"
    });
  }

  if (componentName && (scale?.delta || scale?.absolute || moveDelta || rotateDelta)) {
    actions.push({
      command: "scene.modifyComponent",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        componentName,
        deltaLocation: moveDelta ?? undefined,
        deltaRotation: rotateDelta ?? undefined,
        deltaScale: scale?.delta,
        scale: scale?.absolute
      },
      risk: "low"
    });
  }

  if (materialAssignment) {
    actions.push({
      command: "scene.setComponentMaterial",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        componentName: materialAssignment.componentName,
        materialPath: materialAssignment.materialPath,
        materialSlot: materialAssignment.materialSlot
      },
      risk: "low"
    });
  }

  if (meshAssignment) {
    actions.push({
      command: "scene.setComponentStaticMesh",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        componentName: meshAssignment.componentName,
        meshPath: meshAssignment.meshPath
      },
      risk: "low"
    });
  }

  if (tag) {
    actions.push({
      command: "scene.addActorTag",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        tag
      },
      risk: "low"
    });
  }

  if (folderPath !== null) {
    actions.push({
      command: "scene.setActorFolder",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        folderPath
      },
      risk: "low"
    });
  }

  if (labelPrefix) {
    actions.push({
      command: "scene.addActorLabelPrefix",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        prefix: labelPrefix
      },
      risk: "low"
    });
  }

  if (duplicate) {
    actions.push({
      command: "scene.duplicateActors",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        count: duplicate.count,
        offset: duplicate.offset
      },
      risk: duplicate.count > 5 ? "medium" : "low"
    });
  }

  if (actions.length === 0) {
    actions.push(...buildContextActionsFromPrompt(input.prompt));
  }

  const hasHighRiskAction = actions.some((action) => action.risk === "high");
  const hasWriteActions = actions.some(
    (action) => action.command !== "context.getSceneSummary" && action.command !== "context.getSelection"
  );
  const goal = buildGoal(metadata.goalType, input.prompt);
  const checks = buildChecks(metadata, hasHighRiskAction);
  const stopConditions = buildStopConditions(hasHighRiskAction);

  if (actions.length > 0) {
    const steps = !hasWriteActions
      ? ["Collect requested context", "Return context result to user", "Wait for next instruction"]
      : [
          "Preview parsed actions",
          "Wait for user approval",
          "Apply approved scene actions with transaction (undo-safe)"
        ];
    return {
      summary: buildActionSummary(input, actions),
      steps,
      actions,
      goal,
      subgoals: buildSubgoals(true, hasWriteActions),
      checks,
      stopConditions
    };
  }

  return {
    summary: `Draft plan for: ${input.prompt} (selected: ${selectionNames.length})`,
    steps: [
      "Collect scene context",
      "Build action list",
      "No executable action parsed from prompt yet"
    ],
    actions: [],
    goal,
    subgoals: buildSubgoals(false, false),
    checks,
    stopConditions
  };
}
