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
  if (
    !/(create|spawn|add)/.test(lower) ||
    !/(actor|actors|object|mesh|cube|sphere|light|camera|fog|post process|postprocess|ppv)/.test(lower)
  ) {
    return null;
  }

  let actorClass = "Actor";
  if (/(directional light|sun light|sun)/.test(lower)) {
    actorClass = "DirectionalLight";
  } else if (/(spot light)/.test(lower)) {
    actorClass = "SpotLight";
  } else if (/(point light|light)/.test(lower)) {
    actorClass = "PointLight";
  } else if (/(fog|exponential height fog)/.test(lower)) {
    actorClass = "ExponentialHeightFog";
  } else if (/(post process volume|postprocess volume|ppv|post process)/.test(lower)) {
    actorClass = "PostProcessVolume";
  } else if (/(mesh|cube|static mesh)/.test(lower)) {
    actorClass = "StaticMeshActor";
  } else if (/(sphere)/.test(lower)) {
    actorClass = "StaticMeshActor";
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

function parseDirectionalLightIntensityFromPrompt(prompt: string): number | null {
  const lower = prompt.toLowerCase();
  if (!/(directional light|sun light|sun)/.test(lower)) {
    return null;
  }
  if (!/(intensity|lux|brightness)/.test(lower)) {
    return null;
  }

  const match =
    /\b(?:intensity|lux|brightness)\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt) ??
    /\bset\s+(?:directional light|sun light|sun)\s+(?:intensity|lux|brightness)\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  const value = match ? Number(match[1]) : Number.NaN;
  return Number.isFinite(value) ? value : null;
}

function parseFogDensityFromPrompt(prompt: string): number | null {
  const lower = prompt.toLowerCase();
  if (!/(fog|exponential height fog)/.test(lower)) {
    return null;
  }
  if (!/(density|thickness)/.test(lower)) {
    return null;
  }

  const match =
    /\b(?:fog\s+)?(?:density|thickness)\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt) ??
    /\bset\s+(?:fog|exponential height fog)\s+(?:density|thickness)\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  const value = match ? Number(match[1]) : Number.NaN;
  return Number.isFinite(value) ? value : null;
}

function parsePostProcessExposureCompensationFromPrompt(prompt: string): number | null {
  const lower = prompt.toLowerCase();
  if (!/(exposure|auto exposure)/.test(lower)) {
    return null;
  }
  if (!/(compensation|bias)/.test(lower) && !/(post process|postprocess|ppv)/.test(lower)) {
    return null;
  }

  const match =
    /\b(?:exposure(?:\s+compensation)?|auto exposure(?:\s+bias)?)\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt) ??
    /\b(?:compensation|bias)\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  const value = match ? Number(match[1]) : Number.NaN;
  return Number.isFinite(value) ? value : null;
}

function parseNormalizedPercentValue(prompt: string, label: string): number | null {
  const pattern = new RegExp(`\\b${label}\\s*(?:to|=|by)?\\s*([+-]?\\d+(?:\\.\\d+)?)(\\s*%)?`, "i");
  const match = pattern.exec(prompt);
  if (!match) {
    return null;
  }

  const rawValue = Number(match[1]);
  if (!Number.isFinite(rawValue)) {
    return null;
  }

  if (match[2] || rawValue > 1) {
    return rawValue / 100;
  }

  return rawValue;
}

function parseLandscapeCenterFromPrompt(prompt: string): { x: number; y: number } | null {
  const explicitAxes =
    /\b(?:center|at)\s*x\s*([+-]?\d+(?:\.\d+)?)\s*y\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt) ??
    /\b(?:center|at)\s*\(?\s*([+-]?\d+(?:\.\d+)?)\s*[, ]\s*([+-]?\d+(?:\.\d+)?)\s*\)?/i.exec(prompt);
  if (explicitAxes) {
    const x = Number(explicitAxes[1]);
    const y = Number(explicitAxes[2]);
    if (Number.isFinite(x) && Number.isFinite(y)) {
      return { x, y };
    }
  }

  const axisValues = parseAxisValues(prompt.toLowerCase(), ["x", "y"]);
  if (typeof axisValues.x === "number" && typeof axisValues.y === "number") {
    return { x: axisValues.x, y: axisValues.y };
  }

  return null;
}

function parseLandscapeSizeFromPrompt(prompt: string): { x: number; y: number } | null {
  const areaPattern =
    /\b(?:size|bounds|area)\s*x\s*([+-]?\d+(?:\.\d+)?)\s*y\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt) ??
    /\b(?:width)\s*([+-]?\d+(?:\.\d+)?)\s*(?:[, ]+|\s+)(?:height)\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  if (areaPattern) {
    const sx = Math.abs(Number(areaPattern[1]));
    const sy = Math.abs(Number(areaPattern[2]));
    if (Number.isFinite(sx) && Number.isFinite(sy) && sx > 0 && sy > 0) {
      return { x: sx, y: sy };
    }
  }

  const radiusMatch = /\bradius\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  if (radiusMatch) {
    const radius = Math.abs(Number(radiusMatch[1]));
    if (Number.isFinite(radius) && radius > 0) {
      return { x: radius * 2, y: radius * 2 };
    }
  }

  const uniformMatch =
    /\bbrush(?:\s+size)?\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt) ??
    /\bsize\s*(?:to|=|by)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  if (uniformMatch) {
    const size = Math.abs(Number(uniformMatch[1]));
    if (Number.isFinite(size) && size > 0) {
      return { x: size, y: size };
    }
  }

  return null;
}

function parseLandscapeLayerNameFromPrompt(prompt: string): string | null {
  const match =
    /\blayer\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\blayer\s+\'([^\']+)\'/i.exec(prompt) ??
    /\blayer\s+([0-9A-Za-z_]+)/i.exec(prompt);
  if (!match) {
    return null;
  }

  const layerName = match[1]?.trim();
  return layerName ? layerName : null;
}

function extractPromptSegment(prompt: string, startRegex: RegExp, stopRegex: RegExp): string | null {
  const startMatch = startRegex.exec(prompt);
  if (!startMatch || startMatch.index === undefined || startMatch.index < 0) {
    return null;
  }

  const startIndex = startMatch.index;
  const tail = prompt.slice(startIndex);
  const stopMatch = stopRegex.exec(tail);
  if (!stopMatch || stopMatch.index === undefined || stopMatch.index < 0) {
    return prompt.slice(startIndex);
  }

  const stopIndex = startIndex + stopMatch.index;
  if (stopIndex <= startIndex) {
    return prompt.slice(startIndex);
  }
  return prompt.slice(startIndex, stopIndex);
}

function parseLandscapeSculptFromPrompt(prompt: string): {
  center: { x: number; y: number };
  size: { x: number; y: number };
  strength: number;
  falloff: number;
  mode: "raise" | "lower";
} | null {
  const segment = extractPromptSegment(
    prompt,
    /\b(sculpt|raise terrain|lower terrain|dig terrain|carve terrain)\b/i,
    /\b(paint|texture)\b/i
  );
  if (!segment) {
    return null;
  }

  const lower = segment.toLowerCase();
  const center = parseLandscapeCenterFromPrompt(segment);
  const size = parseLandscapeSizeFromPrompt(segment);
  if (!center || !size) {
    return null;
  }

  const parsedStrength = parseNormalizedPercentValue(segment, "strength");
  const parsedFalloff = parseNormalizedPercentValue(segment, "falloff");
  const strength = Math.max(0, Math.min(1, parsedStrength ?? 0.2));
  const falloff = Math.max(0, Math.min(1, parsedFalloff ?? 0.5));
  const mode: "raise" | "lower" = /(lower|dig|carve|erode|down)/.test(lower) ? "lower" : "raise";
  return { center, size, strength, falloff, mode };
}

function parseLandscapePaintFromPrompt(prompt: string): {
  center: { x: number; y: number };
  size: { x: number; y: number };
  layerName: string;
  strength: number;
  falloff: number;
  mode: "add" | "remove";
} | null {
  const segment = extractPromptSegment(
    prompt,
    /\b(paint|texture)\b/i,
    /\b(sculpt|raise terrain|lower terrain|dig terrain|carve terrain)\b/i
  );
  if (!segment) {
    return null;
  }

  const lower = segment.toLowerCase();
  if (!/\blayer\b/.test(lower)) {
    return null;
  }

  const center = parseLandscapeCenterFromPrompt(segment);
  const size = parseLandscapeSizeFromPrompt(segment);
  const layerName = parseLandscapeLayerNameFromPrompt(segment);
  if (!center || !size || !layerName) {
    return null;
  }

  const parsedStrength = parseNormalizedPercentValue(segment, "strength");
  const parsedFalloff = parseNormalizedPercentValue(segment, "falloff");
  const strength = Math.max(0, Math.min(1, parsedStrength ?? 0.4));
  const falloff = Math.max(0, Math.min(1, parsedFalloff ?? 0.5));
  const mode: "add" | "remove" = /(remove|erase|clear|subtract)/.test(lower) ? "remove" : "add";
  return { center, size, layerName, strength, falloff, mode };
}

function parseLandscapeFeatureSizeRange(
  prompt: string,
  featureRegex: string,
  maxValue: number
): { min?: number; max?: number } {
  let minValue: number | undefined;
  let maxValueParsed: number | undefined;

  const rangeMatch =
    new RegExp(
      `\\b${featureRegex}\\s*(?:width|size)\\s*(?:between|from)\\s*([+-]?\\d+(?:\\.\\d+)?)\\s*(?:and|to)\\s*([+-]?\\d+(?:\\.\\d+)?)\\b`,
      "i"
    ).exec(prompt) ??
    new RegExp(
      `\\b(?:width|size)\\s*of\\s*${featureRegex}\\s*(?:between|from)\\s*([+-]?\\d+(?:\\.\\d+)?)\\s*(?:and|to)\\s*([+-]?\\d+(?:\\.\\d+)?)\\b`,
      "i"
    ).exec(prompt) ??
    new RegExp(
      `\\b${featureRegex}\\s*(?:width|size)\\s*([+-]?\\d+(?:\\.\\d+)?)\\s*(?:-|–|—|to)\\s*([+-]?\\d+(?:\\.\\d+)?)\\b`,
      "i"
    ).exec(prompt) ??
    new RegExp(
      `\\b(?:width|size)\\s*of\\s*${featureRegex}\\s*([+-]?\\d+(?:\\.\\d+)?)\\s*(?:-|–|—|to)\\s*([+-]?\\d+(?:\\.\\d+)?)\\b`,
      "i"
    ).exec(prompt);
  if (rangeMatch) {
    const first = Number(rangeMatch[1]);
    const second = Number(rangeMatch[2]);
    if (Number.isFinite(first) && Number.isFinite(second) && first > 0 && second > 0) {
      minValue = Math.max(1, Math.min(maxValue, Math.min(first, second)));
      maxValueParsed = Math.max(1, Math.min(maxValue, Math.max(first, second)));
    }
  } else {
    const minMatch =
      new RegExp(
        `\\b(?:min(?:imum)?|at least)\\s*(?:${featureRegex}\\s*)?(?:width|size)\\s*(?:of\\s*)?([+-]?\\d+(?:\\.\\d+)?)\\b`,
        "i"
      ).exec(prompt) ??
      new RegExp(`\\b${featureRegex}\\s*(?:width|size)\\s*min(?:imum)?\\s*(?:to|=|:)?\\s*([+-]?\\d+(?:\\.\\d+)?)\\b`, "i").exec(prompt);
    const maxMatch =
      new RegExp(
        `\\b(?:max(?:imum)?|at most|up to|no more than)\\s*(?:${featureRegex}\\s*)?(?:width|size)\\s*(?:of\\s*)?([+-]?\\d+(?:\\.\\d+)?)\\b`,
        "i"
      ).exec(prompt) ??
      new RegExp(`\\b${featureRegex}\\s*(?:width|size)\\s*max(?:imum)?\\s*(?:to|=|:)?\\s*([+-]?\\d+(?:\\.\\d+)?)\\b`, "i").exec(prompt);

    const parsedMin = minMatch ? Number(minMatch[1]) : Number.NaN;
    const parsedMax = maxMatch ? Number(maxMatch[1]) : Number.NaN;
    if (Number.isFinite(parsedMin) && parsedMin > 0) {
      minValue = Math.max(1, Math.min(maxValue, parsedMin));
    }
    if (Number.isFinite(parsedMax) && parsedMax > 0) {
      maxValueParsed = Math.max(1, Math.min(maxValue, parsedMax));
    }

    if (minValue === undefined && maxValueParsed === undefined) {
      const exactMatch = new RegExp(
        `\\b${featureRegex}\\s*(?:width|size)\\s*(?:to|=|:)?\\s*([+-]?\\d+(?:\\.\\d+)?)\\b`,
        "i"
      ).exec(prompt);
      const parsedExact = exactMatch ? Number(exactMatch[1]) : Number.NaN;
      if (Number.isFinite(parsedExact) && parsedExact > 0) {
        const clamped = Math.max(1, Math.min(maxValue, parsedExact));
        minValue = clamped;
        maxValueParsed = clamped;
      }
    }
  }

  if (minValue !== undefined && maxValueParsed !== undefined && minValue > maxValueParsed) {
    return { min: maxValueParsed, max: minValue };
  }
  return { min: minValue, max: maxValueParsed };
}

function parseLandscapeGenerateFromPrompt(prompt: string): {
  theme: "moon_surface" | "nature_island";
  detailLevel: "low" | "medium" | "high" | "cinematic";
  moonProfile?: "moon_surface";
  mountainStyle?: "sharp_peaks" | "hills";
  useFullArea: boolean;
  center?: { x: number; y: number };
  size?: { x: number; y: number };
  seed?: number;
  mountainCount?: number;
  mountainWidthMin?: number;
  mountainWidthMax?: number;
  maxHeight?: number;
  craterCountMin?: number;
  craterCountMax?: number;
  craterWidthMin?: number;
  craterWidthMax?: number;
} | null {
  const lower = prompt.toLowerCase();
  const hasLandscapeHint = /(landscape|terrain)/.test(lower);
  const hasThemeHint = /(moon|lunar|crater|island|nature|natural)/.test(lower);
  const hasGenerationIntent = /(create|build|generate|make|design|craft|form)/.test(lower);
  if (!hasGenerationIntent && !hasThemeHint) {
    return null;
  }
  if (!hasLandscapeHint && !hasThemeHint) {
    return null;
  }

  let theme: "moon_surface" | "nature_island" | null = null;
  if (/(moon|lunar|crater|moon surface)/.test(lower)) {
    theme = "moon_surface";
  } else if (/(island|nature|natural)/.test(lower)) {
    theme = "nature_island";
  }
  if (!theme) {
    return null;
  }

  let moonProfile: "moon_surface" | undefined;
  if (theme === "moon_surface") {
    const explicitAncient =
      /\bmoon\s*profile\s*(?:to|=|:)?\s*(?:ancient|ancient[_\s-]heavily[_\s-]cratered|heavily[_\s-]cratered)\b/i.test(prompt) ||
      /\bancient[_\s-]heavily[_\s-]cratered\b/i.test(prompt);
    const ancientCueCount =
      Number(/\bancient\b/i.test(prompt)) +
      Number(/\bheavily\s+cratered\b/i.test(prompt)) +
      Number(/\boverlapping\s+impact\s+craters?\b/i.test(prompt)) +
      Number(/\bregolith\b/i.test(prompt)) +
      Number(/\bejecta\b/i.test(prompt)) +
      Number(/\bweathered\b/i.test(prompt)) +
      Number(/\bterraces?\b/i.test(prompt));
    if (explicitAncient || ancientCueCount >= 2) {
      moonProfile = "moon_surface";
    } else {
      // Ancient heavily cratered is the default moon profile.
      moonProfile = "moon_surface";
    }
  }

  const explicitDetailMatch = /\bdetail(?:\s*level)?\s*(?:to|=|:)?\s*(low|medium|high|cinematic)\b/i.exec(prompt);
  const explicitDetailLevel = explicitDetailMatch?.[1]?.toLowerCase();
  let detailLevel: "low" | "medium" | "high" | "cinematic" | undefined;
  if (explicitDetailLevel === "low" || explicitDetailLevel === "medium" || explicitDetailLevel === "high" || explicitDetailLevel === "cinematic") {
    detailLevel = explicitDetailLevel;
  } else if (/\b(cinematic|ultra|photoreal(?:istic)?)\b/i.test(prompt)) {
    detailLevel = "cinematic";
  } else if (/\b(realistic|high[\s-]*detail|detailed|fine detail|rich detail)\b/i.test(prompt)) {
    detailLevel = "high";
  } else if (/\b(stylized|stylised|simple|minimal|low[\s-]*detail|low[\s-]*poly)\b/i.test(prompt)) {
    detailLevel = "low";
  }
  if (!detailLevel) {
    detailLevel = theme === "moon_surface" ? "high" : "medium";
  }
  if (moonProfile === "moon_surface" && detailLevel === "medium") {
    detailLevel = "high";
  }

  let center = parseLandscapeCenterFromPrompt(prompt) ?? undefined;
  let size = parseLandscapeSizeFromPrompt(prompt) ?? undefined;
  let hasAreaBounds = Boolean(center && size);
  const useFullAreaHint = /(all available space|entire landscape|whole landscape|full landscape|across the landscape|all of the landscape)/.test(
    lower
  );
  let useFullArea = useFullAreaHint || !hasAreaBounds;

  const seedMatch = /\bseed\s*(?:to|=|:)?\s*(-?\d+)\b/i.exec(prompt);
  const parsedSeed = seedMatch ? Number(seedMatch[1]) : Number.NaN;
  const seed = Number.isInteger(parsedSeed) ? parsedSeed : undefined;

  const hasHillHint = /\bhills?\b/i.test(prompt);
  const hasSharpPeakHint = /\b(sharp\s+peaks?|sharp\s+(?:mountains?|mountians?)|spiky\s+(?:mountains?|mountians?)|jagged\s+(?:mountains?|mountians?))\b/i.test(
    prompt
  );
  const mountainStyle =
    theme === "nature_island"
      ? hasHillHint && !hasSharpPeakHint
        ? "hills"
        : "sharp_peaks"
      : undefined;

  const mountainMatch =
    /\b(\d+)\s+(?:mountains?|mountians?|hills?)\b/i.exec(prompt) ??
    /\b(?:mountains?|mountians?|hills?)\s*(?:count|num(?:ber)?)?\s*(?:to|=|:)?\s*(\d+)\b/i.exec(prompt);
  const parsedMountainCount = mountainMatch ? Number(mountainMatch[1]) : Number.NaN;
  const explicitMountainCount = Number.isFinite(parsedMountainCount)
    ? Math.max(1, Math.min(8, Math.trunc(parsedMountainCount)))
    : undefined;
  const singleMountainHint = /\b(?:a|an|one)\s+(?:mountain|mountian|hill)\b/i.test(prompt);
  const inferredMoonDensity =
    detailLevel === "cinematic"
      ? 8
      : detailLevel === "high"
      ? 6
      : detailLevel === "medium"
      ? 4
      : 2;
  const mountainCount =
    explicitMountainCount ??
    (theme === "nature_island" && singleMountainHint ? 1 : theme === "moon_surface" ? inferredMoonDensity : undefined);

  const mountainWidthRange = parseLandscapeFeatureSizeRange(prompt, "(?:mountains?|mountians?|hills?)", 200000);
  let mountainWidthMin = mountainWidthRange.min;
  let mountainWidthMax = mountainWidthRange.max;
  if (
    mountainWidthMin === undefined &&
    mountainWidthMax === undefined &&
    (hasHillHint || /\b(?:mountains?|mountians?)\b/i.test(prompt))
  ) {
    const genericWidthRangeMatch =
      /\bwidth\s*(?:between|from)?\s*([+-]?\d+(?:\.\d+)?)\s*(?:-|–|—|and|to)\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt);
    if (genericWidthRangeMatch) {
      const first = Number(genericWidthRangeMatch[1]);
      const second = Number(genericWidthRangeMatch[2]);
      if (Number.isFinite(first) && Number.isFinite(second) && first > 0 && second > 0) {
        mountainWidthMin = Math.max(1, Math.min(200000, Math.min(first, second)));
        mountainWidthMax = Math.max(1, Math.min(200000, Math.max(first, second)));
      }
    }
  }
  if (theme === "nature_island" && mountainStyle === "hills" && center && !size) {
    const explicitWidth = mountainWidthMax ?? mountainWidthMin;
    const derivedSpan = Math.max(1000, Math.min(200000, (explicitWidth ?? 2000) * 3));
    size = { x: derivedSpan, y: derivedSpan };
    hasAreaBounds = true;
    useFullArea = false;
  }

  const maxHeightMatch =
    /\bmax(?:imum)?\s*height(?:\s*of)?\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt) ??
    /\bheight\s*(?:max(?:imum)?|limit)\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt);
  const parsedMaxHeight = maxHeightMatch ? Number(maxHeightMatch[1]) : Number.NaN;
  let maxHeight = Number.isFinite(parsedMaxHeight) && parsedMaxHeight > 0 ? parsedMaxHeight : undefined;
  if (maxHeight === undefined && theme === "nature_island") {
    const heightRangeMatch =
      /\b(?:hills?|mountains?|mountians?)?\s*height\s*(?:between|from)?\s*([+-]?\d+(?:\.\d+)?)\s*(?:-|–|—|and|to)\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(
        prompt
      );
    if (heightRangeMatch) {
      const first = Number(heightRangeMatch[1]);
      const second = Number(heightRangeMatch[2]);
      if (Number.isFinite(first) && Number.isFinite(second) && first > 0 && second > 0) {
        maxHeight = Math.max(first, second);
      }
    } else {
      const heightExactMatch = /\b(?:hills?|mountains?|mountians?)?\s*height\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt);
      const parsedHeight = heightExactMatch ? Number(heightExactMatch[1]) : Number.NaN;
      if (Number.isFinite(parsedHeight) && parsedHeight > 0) {
        maxHeight = parsedHeight;
      }
    }
  }

  let craterCountMin: number | undefined;
  let craterCountMax: number | undefined;
  const craterCountRangeMatch =
    /\b(?:between|from)\s*(\d+)\s*(?:and|to)\s*(\d+)\s+(?:craters?|crators?)\b/i.exec(prompt) ??
    /\b(?:craters?|crators?)\s*(?:count)?\s*(?:between|from)\s*(\d+)\s*(?:and|to)\s*(\d+)\b/i.exec(prompt);
  if (craterCountRangeMatch) {
    const first = Number(craterCountRangeMatch[1]);
    const second = Number(craterCountRangeMatch[2]);
    if (Number.isFinite(first) && Number.isFinite(second)) {
      craterCountMin = Math.max(1, Math.min(500, Math.trunc(Math.min(first, second))));
      craterCountMax = Math.max(1, Math.min(500, Math.trunc(Math.max(first, second))));
    }
  } else {
    const craterCountMinMatch =
      /\b(?:min(?:imum)?|at least)\s*(?:(?:craters?|crators?)\s*)?(?:count\s*)?(?:of\s*)?(\d+)\b/i.exec(prompt) ??
      /\b(?:craters?|crators?)\s*(?:count\s*)?min(?:imum)?\s*(?:to|=|:)?\s*(\d+)\b/i.exec(prompt);
    const craterCountMaxMatch =
      /\b(?:max(?:imum)?|at most|up to|no more than)\s*(?:(?:craters?|crators?)\s*)?(?:count\s*)?(?:of\s*)?(\d+)\b/i.exec(prompt) ??
      /\b(?:craters?|crators?)\s*(?:count\s*)?max(?:imum)?\s*(?:to|=|:)?\s*(\d+)\b/i.exec(prompt);

    const parsedCraterCountMin = craterCountMinMatch ? Number(craterCountMinMatch[1]) : Number.NaN;
    const parsedCraterCountMax = craterCountMaxMatch ? Number(craterCountMaxMatch[1]) : Number.NaN;
    if (Number.isFinite(parsedCraterCountMin)) {
      craterCountMin = Math.max(1, Math.min(500, Math.trunc(parsedCraterCountMin)));
    }
    if (Number.isFinite(parsedCraterCountMax)) {
      craterCountMax = Math.max(1, Math.min(500, Math.trunc(parsedCraterCountMax)));
    }

    if (craterCountMin === undefined && craterCountMax === undefined) {
      const craterCountExactMatch =
        /\b(?:craters?|crators?)\s*(?:count\s*)?(?:to|=|:)\s*(\d+)\b/i.exec(prompt) ??
        /\b(\d+)\s+(?:craters?|crators?)\b/i.exec(prompt);
      const parsedCraterCount = craterCountExactMatch ? Number(craterCountExactMatch[1]) : Number.NaN;
      if (Number.isFinite(parsedCraterCount)) {
        const clamped = Math.max(1, Math.min(500, Math.trunc(parsedCraterCount)));
        craterCountMin = clamped;
        craterCountMax = clamped;
      }
    }
  }
  if (
    craterCountMin !== undefined &&
    craterCountMax !== undefined &&
    craterCountMin > craterCountMax
  ) {
    const swap = craterCountMin;
    craterCountMin = craterCountMax;
    craterCountMax = swap;
  }

  let craterWidthMin: number | undefined;
  let craterWidthMax: number | undefined;
  const craterWidthRangeMatch =
    /\b(?:craters?|crators?)\s*width\s*(?:between|from)\s*([+-]?\d+(?:\.\d+)?)\s*(?:and|to)\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt) ??
    /\bwidth\s*of\s*(?:craters?|crators?)\s*(?:between|from)\s*([+-]?\d+(?:\.\d+)?)\s*(?:and|to)\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt);
  if (craterWidthRangeMatch) {
    const first = Number(craterWidthRangeMatch[1]);
    const second = Number(craterWidthRangeMatch[2]);
    if (Number.isFinite(first) && Number.isFinite(second) && first > 0 && second > 0) {
      craterWidthMin = Math.max(1, Math.min(200000, Math.min(first, second)));
      craterWidthMax = Math.max(1, Math.min(200000, Math.max(first, second)));
    }
  } else {
    const craterWidthMinMatch =
      /\b(?:min(?:imum)?|at least)\s*(?:(?:craters?|crators?)\s*)?width\s*(?:of\s*)?([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt) ??
      /\bmin(?:imum)?\s*width(?:\s*of)?\s*(?:craters?|crators?)\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt) ??
      /\b(?:craters?|crators?)\s*width\s*min(?:imum)?\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt);
    const craterWidthMaxMatch =
      /\b(?:max(?:imum)?|at most|up to|no more than)\s*(?:(?:craters?|crators?)\s*)?width\s*(?:of\s*)?([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt) ??
      /\bmax(?:imum)?\s*width(?:\s*of)?\s*(?:craters?|crators?)\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt) ??
      /\b(?:craters?|crators?)\s*width\s*max(?:imum)?\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)\b/i.exec(prompt);

    const parsedCraterWidthMin = craterWidthMinMatch ? Number(craterWidthMinMatch[1]) : Number.NaN;
    const parsedCraterWidthMax = craterWidthMaxMatch ? Number(craterWidthMaxMatch[1]) : Number.NaN;
    if (Number.isFinite(parsedCraterWidthMin) && parsedCraterWidthMin > 0) {
      craterWidthMin = Math.max(1, Math.min(200000, parsedCraterWidthMin));
    }
    if (Number.isFinite(parsedCraterWidthMax) && parsedCraterWidthMax > 0) {
      craterWidthMax = Math.max(1, Math.min(200000, parsedCraterWidthMax));
    }
  }
  if (
    craterWidthMin !== undefined &&
    craterWidthMax !== undefined &&
    craterWidthMin > craterWidthMax
  ) {
    const swap = craterWidthMin;
    craterWidthMin = craterWidthMax;
    craterWidthMax = swap;
  }
  if (theme === "moon_surface" && moonProfile === "moon_surface") {
    if (craterCountMin === undefined) {
      craterCountMin = 140;
    }
    if (craterCountMax === undefined) {
      craterCountMax = 340;
    }
  }

  return {
    theme,
    detailLevel,
    moonProfile,
    mountainStyle,
    useFullArea,
    center: useFullArea ? undefined : center,
    size: useFullArea ? undefined : size,
    seed,
    mountainCount,
    mountainWidthMin,
    mountainWidthMax,
    maxHeight,
    craterCountMin,
    craterCountMax,
    craterWidthMin,
    craterWidthMax
  };
}

function sanitizeGraphAssetName(input: string): string {
  const sanitized = input
    .trim()
    .replace(/[^a-zA-Z0-9_]+/g, "_")
    .replace(/^_+|_+$/g, "");
  return sanitized.length > 0 ? sanitized : "PCG_Graph";
}

function parsePcgGraphPathFromPrompt(prompt: string): string | null {
  const objectPathMatch = /((?:\/Game\/)[0-9A-Za-z_\/]+)\.([0-9A-Za-z_]+)/.exec(prompt);
  if (objectPathMatch?.[1]) {
    return objectPathMatch[1];
  }

  const packagePathMatch = /(\/Game\/[0-9A-Za-z_\/]+)/.exec(prompt);
  if (packagePathMatch?.[1]) {
    return packagePathMatch[1];
  }

  const namedMatch =
    /\b(?:named|name)\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\b(?:named|name)\s+\'([^\']+)\'/i.exec(prompt) ??
    /\b(?:named|name)\s+([0-9A-Za-z_]+)/i.exec(prompt);
  if (namedMatch?.[1]) {
    return `/Game/PCG/${sanitizeGraphAssetName(namedMatch[1])}`;
  }

  return null;
}

function parsePcgCreateGraphFromPrompt(prompt: string): { assetPath: string; templatePath?: string; overwrite: boolean } | null {
  const lower = prompt.toLowerCase();
  if (!/(pcg|procedural content generation)/.test(lower)) {
    return null;
  }

  const hasTemplateHint = /\b(template|built[\s-]*in)\b/.test(lower);
  const hasGrassHint = /\bgrass\b/.test(lower);
  const isRuntimeGrassGpuIntent =
    /(runtime\s+grass\s+gpu|runtime\s+gpu\s+grass)/.test(lower) ||
    /tpl_showcase_runtimegrassgpu/.test(lower) ||
    (hasGrassHint && hasTemplateHint);

  if (!/(create|make|new|build|generate)/.test(lower)) {
    return null;
  }

  if (!/(graph|pcg graph)/.test(lower) && !isRuntimeGrassGpuIntent) {
    return null;
  }

  const templateMatch =
    /\b(?:from|using)\s+template\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\b(?:from|using)\s+template\s+\'([^\']+)\'/i.exec(prompt) ??
    /\b(?:from|using)\s+template\s+([^\s,]+)/i.exec(prompt) ??
    /\b(?:form)\s+template\s+\"([^\"]+)\"/i.exec(prompt) ??
    /\b(?:form)\s+template\s+\'([^\']+)\'/i.exec(prompt) ??
    /\b(?:form)\s+template\s+([^\s,]+)/i.exec(prompt);
  const templatePath = templateMatch?.[1]?.trim();
  const promptWithoutTemplate = prompt.replace(
    /\b(?:from|form|using)\s+(?:built[\s-]*in\s+)?template\s+(?:\"[^\"]+\"|\'[^\']+\'|[^\s,]+)/i,
    " "
  );

  const namedDestinationMatch =
    /\b(?:named|name)\s+\"([^\"]+)\"/i.exec(promptWithoutTemplate) ??
    /\b(?:named|name)\s+\'([^\']+)\'/i.exec(promptWithoutTemplate) ??
    /\b(?:named|name)\s+([0-9A-Za-z_]+)/i.exec(promptWithoutTemplate);
  const destinationPathFromName = namedDestinationMatch?.[1]
    ? `/Game/PCG/${sanitizeGraphAssetName(namedDestinationMatch[1])}`
    : null;
  const runtimeGrassTemplateObjectPath = "/PCG/GraphTemplates/TPL_Showcase_RuntimeGrassGPU.TPL_Showcase_RuntimeGrassGPU";
  const inferredTemplatePath = templatePath ?? (isRuntimeGrassGpuIntent ? runtimeGrassTemplateObjectPath : undefined);
  const defaultDestinationPath = isRuntimeGrassGpuIntent ? "/Game/PCG/RuntimeGrassGPU" : "/Game/PCG/PCG_Graph";

  return {
    assetPath: destinationPathFromName ?? parsePcgGraphPathFromPrompt(promptWithoutTemplate) ?? defaultDestinationPath,
    templatePath: inferredTemplatePath && inferredTemplatePath.length > 0 ? inferredTemplatePath : undefined,
    overwrite: /(overwrite|replace|recreate|reset)\b/.test(lower)
  };
}

function parsePcgPlaceOnLandscapeFromPrompt(prompt: string): {
  graphSource: "path" | "last" | "selected";
  graphPath?: string;
  placementMode: "center" | "full";
  size?: { x: number; y: number };
  targetAllLandscapes: boolean;
} | null {
  const lower = prompt.toLowerCase();
  const hasPcgHint = /(pcg|procedural content generation|runtime\s+grass\s+gpu|runtime\s+gpu\s+grass)/.test(lower);
  const hasReferentialObject = /\b(it|them|that|those)\b/.test(lower);
  if (!/(landscape|terrain)/.test(lower)) {
    return null;
  }

  if (!/(place|put|apply|attach|add|spawn|drop)/.test(lower)) {
    return null;
  }
  if (!hasPcgHint && !hasReferentialObject) {
    return null;
  }

  const graphPath = parsePcgGraphPathFromPrompt(prompt) ?? undefined;
  const useSelectedGraph =
    /(selected\s+(?:pcg|graph)|selected\s+one|use\s+selected\s+(?:pcg|graph))/i.test(prompt);
  const useLastGraph =
    /(last\s+(?:pcg|graph|one)|latest\s+(?:pcg|graph)|previous\s+(?:pcg|graph)|recent\s+(?:pcg|graph)|newly\s+created|new\s+created)/i.test(
      prompt
    );
  const graphSource: "path" | "last" | "selected" = useSelectedGraph
    ? "selected"
    : useLastGraph
    ? "last"
    : graphPath
    ? "path"
    : "last";

  const placementMode: "center" | "full" = /(full\s+landscape|whole\s+landscape|entire\s+landscape|full\s+size)/.test(lower)
    ? "full"
    : "center";
  const parsedSize = parseLandscapeSizeFromPrompt(prompt);
  const targetAllLandscapes = /(all\s+landscapes|every\s+landscape|each\s+landscape|across\s+all\s+landscapes)/.test(lower);

  return {
    graphSource,
    graphPath,
    placementMode,
    size: placementMode === "center" && parsedSize ? parsedSize : undefined,
    targetAllLandscapes
  };
}

function parsePcgAddConnectCommonNodesFromPrompt(prompt: string): {
  graphPath: string;
  nodeTypes?: ("surfaceSampler" | "transformPoints")[];
  connectFromInput: boolean;
  connectToOutput: boolean;
} | null {
  const lower = prompt.toLowerCase();
  if (!/(pcg|procedural content generation)/.test(lower) || !/(graph|pcg graph)/.test(lower)) {
    return null;
  }

  const hasAddConnectIntent = /(add|insert|connect|wire|setup|set up)/.test(lower);
  const hasCommonNodeHint = /(common nodes|starter nodes|basic pipeline|typical nodes)/.test(lower);
  if (!hasAddConnectIntent && !hasCommonNodeHint) {
    return null;
  }

  const nodeTypes = new Set<"surfaceSampler" | "transformPoints">();
  if (/(surface sampler|sample surface|surface points)/.test(lower)) {
    nodeTypes.add("surfaceSampler");
  }
  if (/(transform points|offset points|jitter points|transform node)/.test(lower)) {
    nodeTypes.add("transformPoints");
  }

  if (nodeTypes.size === 0 && hasCommonNodeHint) {
    nodeTypes.add("surfaceSampler");
    nodeTypes.add("transformPoints");
  }
  if (nodeTypes.size === 0) {
    return null;
  }

  return {
    graphPath: parsePcgGraphPathFromPrompt(prompt) ?? "/Game/PCG/PCG_Graph",
    nodeTypes: Array.from(nodeTypes),
    connectFromInput: !/(without input|do not connect input|skip input connection)/.test(lower),
    connectToOutput: !/(without output|do not connect output|skip output connection)/.test(lower)
  };
}

function parseVectorAfterLabel(prompt: string, labelPattern: RegExp): { x: number; y: number; z: number } | null {
  const match = labelPattern.exec(prompt);
  if (!match || match.index < 0) {
    return null;
  }

  const tail = prompt.slice(match.index).toLowerCase();
  const values = parseAxisValues(tail, ["x", "y", "z"]);
  if (typeof values.x !== "number" || typeof values.y !== "number" || typeof values.z !== "number") {
    return null;
  }

  return {
    x: values.x,
    y: values.y,
    z: values.z
  };
}

function parsePcgSetKeyParametersFromPrompt(prompt: string): {
  graphPath: string;
  surfacePointsPerSquaredMeter?: number;
  surfaceLooseness?: number;
  surfacePointExtents?: { x: number; y: number; z: number };
  transformOffsetMin?: { x: number; y: number; z: number };
  transformOffsetMax?: { x: number; y: number; z: number };
} | null {
  const lower = prompt.toLowerCase();
  if (!/(pcg|procedural content generation)/.test(lower) || !/(graph|pcg graph)/.test(lower)) {
    return null;
  }
  if (!/(set|update|tune|change|adjust)/.test(lower)) {
    return null;
  }

  const pointsMatch = /\bpoints?\s*per\s*(?:square\s*)?(?:meter|metre|m2)\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  const loosenessMatch = /\blooseness\s*(?:to|=|:)?\s*([+-]?\d+(?:\.\d+)?)/i.exec(prompt);
  const pointExtents = parseVectorAfterLabel(prompt, /\bpoint\s+extents?\b/i);
  const transformOffsetMin = parseVectorAfterLabel(prompt, /\boffset\s*min\b/i);
  const transformOffsetMax = parseVectorAfterLabel(prompt, /\boffset\s*max\b/i);
  const transformOffsetExact = parseVectorAfterLabel(prompt, /\boffset\b/i);

  const parsedPointsPerMeter = pointsMatch ? Number(pointsMatch[1]) : Number.NaN;
  const parsedLooseness = loosenessMatch ? Number(loosenessMatch[1]) : Number.NaN;
  const surfacePointsPerSquaredMeter = Number.isFinite(parsedPointsPerMeter) ? parsedPointsPerMeter : undefined;
  const surfaceLooseness = Number.isFinite(parsedLooseness) ? parsedLooseness : undefined;

  const resolvedOffsetMin = transformOffsetMin ?? (transformOffsetExact && !transformOffsetMax ? transformOffsetExact : undefined);
  const resolvedOffsetMax = transformOffsetMax ?? (transformOffsetExact && !transformOffsetMin ? transformOffsetExact : undefined);

  if (
    surfacePointsPerSquaredMeter === undefined &&
    surfaceLooseness === undefined &&
    !pointExtents &&
    !resolvedOffsetMin &&
    !resolvedOffsetMax
  ) {
    return null;
  }

  return {
    graphPath: parsePcgGraphPathFromPrompt(prompt) ?? "/Game/PCG/PCG_Graph",
    surfacePointsPerSquaredMeter,
    surfaceLooseness,
    surfacePointExtents: pointExtents ?? undefined,
    transformOffsetMin: resolvedOffsetMin,
    transformOffsetMax: resolvedOffsetMax
  };
}

function parseUndoFromPrompt(prompt: string): boolean {
  return /\b(undo|revert|roll\s*back|rollback|go\s+back|ctrl\+?z|control\+?z)\b/i.test(prompt);
}

function parseRedoFromPrompt(prompt: string): boolean {
  return /\b(redo|do\s+again|reapply|ctrl\+?y|control\+?y)\b/i.test(prompt);
}

function hasWriteIntent(prompt: string): boolean {
  return /(move|offset|translate|shift|rotate|turn|spin|scale|resize|grow|shrink|create|spawn|add|build|make|generate|delete|remove|destroy|erase|set|assign|apply|replace|duplicate|copy|clone|paint|sculpt|place|put|attach|drop|undo|revert|rollback|roll back|redo|do again|reapply)/i.test(
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

  if (first.command === "scene.setDirectionalLightIntensity") {
    return `Set directional light intensity to ${formatNumber(first.params.intensity)}.`;
  }

  if (first.command === "scene.setFogDensity") {
    return `Set fog density to ${formatNumber(first.params.density)}.`;
  }

  if (first.command === "scene.setPostProcessExposureCompensation") {
    return `Set exposure compensation to ${formatNumber(first.params.exposureCompensation)}.`;
  }

  if (first.command === "landscape.sculpt") {
    return `Sculpt landscape in bounded area near X=${formatNumber(first.params.center.x)}, Y=${formatNumber(first.params.center.y)}.`;
  }

  if (first.command === "landscape.paintLayer") {
    return `Paint landscape layer "${first.params.layerName}" in bounded area.`;
  }

  if (first.command === "landscape.generate") {
    const moonDetail =
      first.params.theme === "moon_surface" &&
      (first.params.detailLevel === "high" || first.params.detailLevel === "cinematic");
    const moonAncient = first.params.theme === "moon_surface" && first.params.moonProfile === "moon_surface";
    const baseSummary = first.params.theme === "moon_surface"
      ? moonAncient
        ? "Generate ancient heavily cratered moon landscape."
        : moonDetail
        ? "Generate realistic moon-like landscape surface."
        : "Generate moon-like landscape surface."
      : "Generate natural island landscape.";
    return first.params.useFullArea ? baseSummary : `${baseSummary} Use requested bounded area.`;
  }

  if (first.command === "pcg.createGraph") {
    if (first.params.templatePath) {
      return `Create PCG graph asset at ${first.params.assetPath} from template ${first.params.templatePath}.`;
    }
    return `Create PCG graph asset at ${first.params.assetPath}.`;
  }

  if (first.command === "pcg.placeOnLandscape") {
    const areaText = first.params.placementMode === "full" ? "full landscape area" : "landscape center";
    return `Place PCG graph on ${areaText}.`;
  }

  if (first.command === "pcg.addConnectCommonNodes") {
    return `Add and connect common PCG nodes in ${first.params.graphPath}.`;
  }

  if (first.command === "pcg.setKeyParameters") {
    return `Set key PCG parameters in ${first.params.graphPath}.`;
  }

  if (first.command === "editor.undo") {
    return "Undo last editor action.";
  }

  if (first.command === "editor.redo") {
    return "Redo last editor action.";
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
  const directionalLightIntensity = parseDirectionalLightIntensityFromPrompt(input.prompt);
  const fogDensity = parseFogDensityFromPrompt(input.prompt);
  const exposureCompensation = parsePostProcessExposureCompensationFromPrompt(input.prompt);
  const landscapeGenerate = parseLandscapeGenerateFromPrompt(input.prompt);
  const landscapeSculpt = parseLandscapeSculptFromPrompt(input.prompt);
  const landscapePaint = parseLandscapePaintFromPrompt(input.prompt);
  const pcgCreateGraph = parsePcgCreateGraphFromPrompt(input.prompt);
  const pcgPlaceOnLandscape = parsePcgPlaceOnLandscapeFromPrompt(input.prompt);
  const pcgAddConnectCommonNodes = parsePcgAddConnectCommonNodesFromPrompt(input.prompt);
  const pcgSetKeyParameters = parsePcgSetKeyParametersFromPrompt(input.prompt);
  const hasPcgActionIntent = Boolean(pcgCreateGraph || pcgPlaceOnLandscape || pcgAddConnectCommonNodes || pcgSetKeyParameters);
  const undoLastAction = parseUndoFromPrompt(input.prompt);
  const redoLastAction = parseRedoFromPrompt(input.prompt);

  const actions: PlanAction[] = [];
  if (createActor) {
    actions.push({
      command: "scene.createActor",
      params: createActor,
      risk: createActor.count > 10 ? "medium" : "low"
    });
  }

  if (!hasPcgActionIntent && (moveDelta || rotateDelta || scale?.delta || scale?.absolute)) {
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

  if (directionalLightIntensity !== null) {
    actions.push({
      command: "scene.setDirectionalLightIntensity",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        intensity: directionalLightIntensity
      },
      risk: "low"
    });
  }

  if (fogDensity !== null) {
    actions.push({
      command: "scene.setFogDensity",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        density: fogDensity
      },
      risk: "low"
    });
  }

  if (exposureCompensation !== null) {
    actions.push({
      command: "scene.setPostProcessExposureCompensation",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        exposureCompensation
      },
      risk: "low"
    });
  }

  if (landscapeGenerate) {
    actions.push({
      command: "landscape.generate",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        theme: landscapeGenerate.theme,
        detailLevel: landscapeGenerate.detailLevel,
        moonProfile: landscapeGenerate.moonProfile,
        mountainStyle: landscapeGenerate.mountainStyle,
        useFullArea: landscapeGenerate.useFullArea,
        center: landscapeGenerate.useFullArea ? undefined : landscapeGenerate.center,
        size: landscapeGenerate.useFullArea ? undefined : landscapeGenerate.size,
        seed: landscapeGenerate.seed,
        mountainCount: landscapeGenerate.mountainCount,
        mountainWidthMin: landscapeGenerate.mountainWidthMin,
        mountainWidthMax: landscapeGenerate.mountainWidthMax,
        maxHeight: landscapeGenerate.maxHeight,
        craterCountMin: landscapeGenerate.craterCountMin,
        craterCountMax: landscapeGenerate.craterCountMax,
        craterWidthMin: landscapeGenerate.craterWidthMin,
        craterWidthMax: landscapeGenerate.craterWidthMax
      },
      risk: "medium"
    });
  }

  if (landscapeSculpt) {
    actions.push({
      command: "landscape.sculpt",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        center: landscapeSculpt.center,
        size: landscapeSculpt.size,
        strength: landscapeSculpt.strength,
        falloff: landscapeSculpt.falloff,
        mode: landscapeSculpt.mode
      },
      risk: "medium"
    });
  }

  if (landscapePaint) {
    actions.push({
      command: "landscape.paintLayer",
      params: {
        target: resolvedActorNames && resolvedActorNames.length > 0 ? "byName" : "selection",
        actorNames: resolvedActorNames && resolvedActorNames.length > 0 ? resolvedActorNames : undefined,
        center: landscapePaint.center,
        size: landscapePaint.size,
        layerName: landscapePaint.layerName,
        strength: landscapePaint.strength,
        falloff: landscapePaint.falloff,
        mode: landscapePaint.mode
      },
      risk: "medium"
    });
  }

  if (pcgCreateGraph) {
    actions.push({
      command: "pcg.createGraph",
      params: pcgCreateGraph,
      risk: "medium"
    });
  }

  if (pcgPlaceOnLandscape) {
    const target = pcgPlaceOnLandscape.targetAllLandscapes
      ? "all"
      : resolvedActorNames && resolvedActorNames.length > 0
      ? "byName"
      : "selection";
    actions.push({
      command: "pcg.placeOnLandscape",
      params: {
        target,
        actorNames: target === "byName" ? resolvedActorNames ?? undefined : undefined,
        graphSource: pcgPlaceOnLandscape.graphSource,
        graphPath: pcgPlaceOnLandscape.graphPath,
        placementMode: pcgPlaceOnLandscape.placementMode,
        size: pcgPlaceOnLandscape.size
      },
      risk: "medium"
    });
  }

  if (pcgAddConnectCommonNodes) {
    actions.push({
      command: "pcg.addConnectCommonNodes",
      params: pcgAddConnectCommonNodes,
      risk: "medium"
    });
  }

  if (pcgSetKeyParameters) {
    actions.push({
      command: "pcg.setKeyParameters",
      params: pcgSetKeyParameters,
      risk: "medium"
    });
  }

  if (actions.length === 0 && undoLastAction) {
    actions.push({
      command: "editor.undo",
      params: {},
      risk: "low"
    });
  }

  if (actions.length === 0 && redoLastAction) {
    actions.push({
      command: "editor.redo",
      params: {},
      risk: "low"
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
