import type { NormalizedIntent } from "../intent/intentLayer.js";

export interface WorldState {
  snapshotAt: string;
  selection: {
    count: number;
    actorNames: string[];
  };
  environment: {
    mapName?: string;
    levelName?: string;
    gameStyle?: string;
    timeOfDay?: string;
    weather?: string;
  };
  lighting: {
    hasDirectionalLight?: boolean;
    hasSkyLight?: boolean;
    hasFog?: boolean;
    exposureCompensation?: number;
    directionalLightIntensity?: number;
    skyLightIntensity?: number;
    fogDensity?: number;
  };
  materials: {
    styleHint?: string;
    targetMaterialPaths: string[];
    selectedMaterialCount?: number;
  };
  performance: {
    qualityTier?: "low" | "medium" | "high" | "cinematic";
    targetFps?: number;
    maxDrawCalls?: number;
  };
  availableAssets: {
    materialPaths: string[];
    meshPaths: string[];
  };
  notes: string[];
}

function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" ? (value as Record<string, unknown>) : {};
}

function readString(source: Record<string, unknown>, key: string): string | undefined {
  const value = source[key];
  if (typeof value !== "string") {
    return undefined;
  }
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

function readNumber(source: Record<string, unknown>, key: string): number | undefined {
  const value = source[key];
  if (typeof value === "number" && Number.isFinite(value)) {
    return value;
  }
  if (typeof value === "string") {
    const parsed = Number(value);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
  }
  return undefined;
}

function readBoolean(source: Record<string, unknown>, key: string): boolean | undefined {
  const value = source[key];
  if (typeof value === "boolean") {
    return value;
  }
  if (typeof value === "string") {
    const normalized = value.trim().toLowerCase();
    if (normalized === "true") {
      return true;
    }
    if (normalized === "false") {
      return false;
    }
  }
  return undefined;
}

function readStringArray(source: Record<string, unknown>, key: string): string[] {
  const value = source[key];
  if (!Array.isArray(value)) {
    return [];
  }
  const items = value
    .filter((item): item is string => typeof item === "string")
    .map((item) => item.trim())
    .filter((item) => item.length > 0);
  return Array.from(new Set(items));
}

function readQualityTier(source: Record<string, unknown>, key: string): WorldState["performance"]["qualityTier"] {
  const raw = readString(source, key)?.toLowerCase();
  if (raw === "low" || raw === "medium" || raw === "high" || raw === "cinematic") {
    return raw;
  }
  return undefined;
}

export class WorldStateCollector {
  collect(intent: NormalizedIntent): WorldState {
    const context = asRecord(intent.input.context);
    const selection = readStringArray(context, "selection");

    const environment = asRecord(context.environment);
    const lighting = asRecord(context.lighting);
    const materials = asRecord(context.materials);
    const performance = asRecord(context.performance);
    const assets = asRecord(context.assets);

    const worldState: WorldState = {
      snapshotAt: new Date().toISOString(),
      selection: {
        count: selection.length,
        actorNames: selection
      },
      environment: {
        mapName: readString(environment, "mapName") ?? readString(context, "mapName"),
        levelName: readString(environment, "levelName") ?? readString(context, "levelName"),
        gameStyle: readString(environment, "gameStyle") ?? readString(context, "gameStyle"),
        timeOfDay: readString(environment, "timeOfDay") ?? readString(context, "timeOfDay"),
        weather: readString(environment, "weather") ?? readString(context, "weather")
      },
      lighting: {
        hasDirectionalLight: readBoolean(lighting, "hasDirectionalLight"),
        hasSkyLight: readBoolean(lighting, "hasSkyLight"),
        hasFog: readBoolean(lighting, "hasFog"),
        exposureCompensation: readNumber(lighting, "exposureCompensation"),
        directionalLightIntensity: readNumber(lighting, "directionalLightIntensity"),
        skyLightIntensity: readNumber(lighting, "skyLightIntensity"),
        fogDensity: readNumber(lighting, "fogDensity")
      },
      materials: {
        styleHint: readString(materials, "styleHint") ?? readString(context, "materialStyle"),
        targetMaterialPaths: readStringArray(materials, "targetMaterialPaths"),
        selectedMaterialCount: readNumber(materials, "selectedMaterialCount")
      },
      performance: {
        qualityTier: readQualityTier(performance, "qualityTier") ?? readQualityTier(context, "qualityTier"),
        targetFps: readNumber(performance, "targetFps") ?? readNumber(context, "targetFps"),
        maxDrawCalls: readNumber(performance, "maxDrawCalls") ?? readNumber(context, "maxDrawCalls")
      },
      availableAssets: {
        materialPaths: readStringArray(assets, "materialPaths"),
        meshPaths: readStringArray(assets, "meshPaths")
      },
      notes: []
    };

    if (worldState.selection.count === 0) {
      worldState.notes.push("Selection is empty.");
    }
    if (!worldState.environment.gameStyle) {
      worldState.notes.push("Game style is not provided.");
    }
    if (!worldState.environment.mapName && !worldState.environment.levelName) {
      worldState.notes.push("Map or level name is not provided.");
    }

    return worldState;
  }
}
