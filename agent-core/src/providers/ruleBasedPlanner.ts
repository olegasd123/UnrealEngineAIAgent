import type { PlanAction, PlanOutput } from "../contracts.js";
import type { PlanInput } from "./types.js";

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

  const directionalPattern = /\b(up|down|left|right|forward|back|backward)\s*([+-]?\d+(?:\.\d+)?)/gi;
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

export function buildRuleBasedPlan(input: PlanInput): PlanOutput {
  const selection = Array.isArray((input.context as { selection?: unknown }).selection)
    ? ((input.context as { selection: unknown[] }).selection as unknown[])
    : [];
  const moveDelta = parseMoveDeltaFromPrompt(input.prompt);
  const rotateDelta = parseRotateDeltaFromPrompt(input.prompt);
  const scale = parseScaleFromPrompt(input.prompt);
  const createActor = parseCreateActorFromPrompt(input.prompt);
  const actorNames = parseActorNamesFromPrompt(input.prompt);
  const deleteSelection = shouldDeleteSelection(input.prompt);
  const deleteByName = /(delete|remove|destroy|erase)/.test(input.prompt.toLowerCase()) && Boolean(actorNames?.length);
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
    } = actorNames && actorNames.length > 0 ? { target: "byName", actorNames } : { target: "selection" };
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

  if (deleteByName && actorNames) {
    actions.push({
      command: "scene.deleteActor",
      params: { target: "byName", actorNames },
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
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
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
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
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
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
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
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
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
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
        tag
      },
      risk: "low"
    });
  }

  if (folderPath !== null) {
    actions.push({
      command: "scene.setActorFolder",
      params: {
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
        folderPath
      },
      risk: "low"
    });
  }

  if (labelPrefix) {
    actions.push({
      command: "scene.addActorLabelPrefix",
      params: {
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
        prefix: labelPrefix
      },
      risk: "low"
    });
  }

  if (duplicate) {
    actions.push({
      command: "scene.duplicateActors",
      params: {
        target: actorNames && actorNames.length > 0 ? "byName" : "selection",
        actorNames: actorNames && actorNames.length > 0 ? actorNames : undefined,
        count: duplicate.count,
        offset: duplicate.offset
      },
      risk: duplicate.count > 5 ? "medium" : "low"
    });
  }

  if (actions.length > 0) {
    return {
      summary: `Planned scene actions for prompt: ${input.prompt} (selected: ${selection.length})`,
      steps: [
        "Preview parsed actions",
        "Wait for user approval",
        "Apply approved scene actions with transaction (undo-safe)"
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
