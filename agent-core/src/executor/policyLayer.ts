import type { PolicyRuntimeConfig } from "../config.js";
import type { PlanAction } from "../contracts.js";
import type { LocalPolicyDecision, SessionAction } from "../sessions/sessionTypes.js";

const ALLOWED_CREATE_ACTOR_CLASSES = new Set([
  "Actor",
  "StaticMeshActor",
  "PointLight",
  "SpotLight",
  "DirectionalLight",
  "RectLight",
  "SkyLight",
  "ExponentialHeightFog",
  "PostProcessVolume",
  "CameraActor"
]);

function shouldAutoApprove(mode: "chat" | "agent", risk: PlanAction["risk"]): boolean {
  return mode === "agent" && risk === "low";
}

function normalizeAssetPath(path: string): string {
  return path.trim();
}

function isAllowedAssetPath(path: string): boolean {
  const normalized = normalizeAssetPath(path);
  return normalized.startsWith("/Game/") || normalized.startsWith("/Engine/");
}

function estimateTargetCount(action: PlanAction, policy: PolicyRuntimeConfig): number {
  if (
    action.command === "scene.modifyActor" ||
    action.command === "scene.deleteActor" ||
    action.command === "scene.modifyComponent" ||
    action.command === "scene.setComponentMaterial" ||
    action.command === "scene.setComponentStaticMesh" ||
    action.command === "scene.addActorTag" ||
    action.command === "scene.setActorFolder" ||
    action.command === "scene.addActorLabelPrefix" ||
    action.command === "scene.duplicateActors" ||
    action.command === "scene.setDirectionalLightIntensity" ||
    action.command === "scene.setFogDensity" ||
    action.command === "scene.setPostProcessExposureCompensation" ||
    action.command === "landscape.sculpt" ||
    action.command === "landscape.paintLayer" ||
    action.command === "landscape.generate"
  ) {
    if (action.params.target === "byName") {
      return Math.max(1, action.params.actorNames?.length ?? 0);
    }
    return policy.selectionTargetEstimate;
  }
  return 0;
}

function estimateActionChanges(action: PlanAction, policy: PolicyRuntimeConfig): number {
  if (action.command === "scene.createActor") {
    return action.params.count;
  }
  if (action.command === "scene.duplicateActors") {
    return estimateTargetCount(action, policy) * action.params.count;
  }
  if (
    action.command === "scene.modifyActor" ||
    action.command === "scene.deleteActor" ||
    action.command === "scene.modifyComponent" ||
    action.command === "scene.setComponentMaterial" ||
    action.command === "scene.setComponentStaticMesh" ||
    action.command === "scene.addActorTag" ||
    action.command === "scene.setActorFolder" ||
    action.command === "scene.addActorLabelPrefix" ||
    action.command === "scene.setDirectionalLightIntensity" ||
    action.command === "scene.setFogDensity" ||
    action.command === "scene.setPostProcessExposureCompensation"
  ) {
    return estimateTargetCount(action, policy);
  }
  if (action.command === "landscape.sculpt" || action.command === "landscape.paintLayer") {
    const area = Math.abs(action.params.size.x * action.params.size.y);
    return Math.max(1, Math.round(area / 250000));
  }
  if (action.command === "landscape.generate") {
    if (action.params.useFullArea) {
      return 250;
    }
    if (action.params.size) {
      const area = Math.abs(action.params.size.x * action.params.size.y);
      return Math.max(1, Math.round(area / 200000));
    }
    return 200;
  }
  return 0;
}

function requireApproval(
  action: PlanAction,
  reason: string,
  policy: PolicyRuntimeConfig,
  riskOverride: PlanAction["risk"] = "high"
): LocalPolicyDecision {
  return {
    approved: false,
    risk: riskOverride,
    message: reason,
    hardDenied: false,
    estimatedChanges: estimateActionChanges(action, policy)
  };
}

function hardDeny(action: PlanAction, reason: string, policy: PolicyRuntimeConfig): LocalPolicyDecision {
  return {
    approved: false,
    risk: "high",
    message: reason,
    hardDenied: true,
    estimatedChanges: estimateActionChanges(action, policy)
  };
}

function applyLocalPolicy(action: PlanAction, policy: PolicyRuntimeConfig, mode: "chat" | "agent"): LocalPolicyDecision {
  let risk: PlanAction["risk"] = action.risk;
  let approved = shouldAutoApprove(mode, risk);
  let message: string | undefined;

  if (action.command === "scene.createActor") {
    const actorClass = action.params.actorClass;
    if (!ALLOWED_CREATE_ACTOR_CLASSES.has(actorClass)) {
      return requireApproval(
        action,
        `Policy: actorClass '${actorClass}' is not in the allowlist.`,
        policy,
        "high"
      );
    }

    if (action.params.count > policy.maxCreateCount) {
      action.params.count = policy.maxCreateCount;
      const decision = requireApproval(
        action,
        `Policy: create count capped to ${action.params.count}.`,
        policy,
        "medium"
      );
      approved = decision.approved;
      risk = decision.risk;
      message = decision.message;
    }
  }

  if (action.command === "scene.duplicateActors") {
    if (action.params.count > policy.maxDuplicateCount) {
      action.params.count = policy.maxDuplicateCount;
      const decision = requireApproval(
        action,
        `Policy: duplicate count capped to ${action.params.count}.`,
        policy,
        "medium"
      );
      approved = decision.approved;
      risk = decision.risk;
      message = decision.message;
    }
  }

  if (action.command === "scene.setDirectionalLightIntensity") {
    const clamped = Math.max(0, Math.min(200000, action.params.intensity));
    if (clamped !== action.params.intensity) {
      action.params.intensity = clamped;
      const decision = requireApproval(
        action,
        `Policy: directional light intensity clamped to ${action.params.intensity}.`,
        policy,
        "medium"
      );
      approved = decision.approved;
      risk = decision.risk;
      message = decision.message;
    }
  }

  if (action.command === "scene.setFogDensity") {
    const clamped = Math.max(0, Math.min(5, action.params.density));
    if (clamped !== action.params.density) {
      action.params.density = clamped;
      const decision = requireApproval(
        action,
        `Policy: fog density clamped to ${action.params.density}.`,
        policy,
        "medium"
      );
      approved = decision.approved;
      risk = decision.risk;
      message = decision.message;
    }
  }

  if (action.command === "scene.setPostProcessExposureCompensation") {
    const clamped = Math.max(-15, Math.min(15, action.params.exposureCompensation));
    if (clamped !== action.params.exposureCompensation) {
      action.params.exposureCompensation = clamped;
      const decision = requireApproval(
        action,
        `Policy: exposure compensation clamped to ${action.params.exposureCompensation}.`,
        policy,
        "medium"
      );
      approved = decision.approved;
      risk = decision.risk;
      message = decision.message;
    }
  }

  if (action.command === "landscape.sculpt" || action.command === "landscape.paintLayer") {
    const maxBrushSize = Math.max(1, policy.maxLandscapeBrushSize);
    const maxBrushStrength = Math.max(0.01, policy.maxLandscapeBrushStrength);
    const clampedSizeX = Math.max(1, Math.min(maxBrushSize, Math.abs(action.params.size.x)));
    const clampedSizeY = Math.max(1, Math.min(maxBrushSize, Math.abs(action.params.size.y)));
    const clampedStrength = Math.max(0, Math.min(maxBrushStrength, action.params.strength));
    const clampedFalloff = Math.max(0, Math.min(1, action.params.falloff));
    const hadClamp =
      clampedSizeX !== action.params.size.x ||
      clampedSizeY !== action.params.size.y ||
      clampedStrength !== action.params.strength ||
      clampedFalloff !== action.params.falloff;

    if (hadClamp) {
      action.params.size.x = clampedSizeX;
      action.params.size.y = clampedSizeY;
      action.params.strength = clampedStrength;
      action.params.falloff = clampedFalloff;
    }

    const policyMessage = hadClamp
      ? `Policy: landscape brush values were clamped (size<=${maxBrushSize}, strength<=${maxBrushStrength}). Approval is required.`
      : "Policy: landscape edits always require approval.";
    const decision = requireApproval(action, policyMessage, policy, "medium");
    approved = decision.approved;
    risk = decision.risk;
    message = decision.message;
  }

  if (action.command === "landscape.generate") {
    if (typeof action.params.maxHeight === "number") {
      const clampedMaxHeight = Math.max(100, Math.min(10000, action.params.maxHeight));
      if (clampedMaxHeight !== action.params.maxHeight) {
        action.params.maxHeight = clampedMaxHeight;
      }
    }

    if (typeof action.params.mountainCount === "number") {
      const clampedMountainCount = Math.max(1, Math.min(8, action.params.mountainCount));
      if (clampedMountainCount !== action.params.mountainCount) {
        action.params.mountainCount = clampedMountainCount;
      }
    }

    if (typeof action.params.mountainWidthMin === "number") {
      const clampedMountainWidthMin = Math.max(1, Math.min(200000, action.params.mountainWidthMin));
      if (clampedMountainWidthMin !== action.params.mountainWidthMin) {
        action.params.mountainWidthMin = clampedMountainWidthMin;
      }
    }

    if (typeof action.params.mountainWidthMax === "number") {
      const clampedMountainWidthMax = Math.max(1, Math.min(200000, action.params.mountainWidthMax));
      if (clampedMountainWidthMax !== action.params.mountainWidthMax) {
        action.params.mountainWidthMax = clampedMountainWidthMax;
      }
    }

    if (
      typeof action.params.mountainWidthMin === "number" &&
      typeof action.params.mountainWidthMax === "number" &&
      action.params.mountainWidthMin > action.params.mountainWidthMax
    ) {
      const swap = action.params.mountainWidthMin;
      action.params.mountainWidthMin = action.params.mountainWidthMax;
      action.params.mountainWidthMax = swap;
    }

    if (typeof action.params.craterCountMin === "number") {
      const clampedCraterCountMin = Math.max(1, Math.min(500, Math.trunc(action.params.craterCountMin)));
      if (clampedCraterCountMin !== action.params.craterCountMin) {
        action.params.craterCountMin = clampedCraterCountMin;
      }
    }

    if (typeof action.params.craterCountMax === "number") {
      const clampedCraterCountMax = Math.max(1, Math.min(500, Math.trunc(action.params.craterCountMax)));
      if (clampedCraterCountMax !== action.params.craterCountMax) {
        action.params.craterCountMax = clampedCraterCountMax;
      }
    }

    if (
      typeof action.params.craterCountMin === "number" &&
      typeof action.params.craterCountMax === "number" &&
      action.params.craterCountMin > action.params.craterCountMax
    ) {
      const swap = action.params.craterCountMin;
      action.params.craterCountMin = action.params.craterCountMax;
      action.params.craterCountMax = swap;
    }

    if (typeof action.params.craterWidthMin === "number") {
      const clampedCraterWidthMin = Math.max(1, Math.min(200000, action.params.craterWidthMin));
      if (clampedCraterWidthMin !== action.params.craterWidthMin) {
        action.params.craterWidthMin = clampedCraterWidthMin;
      }
    }

    if (typeof action.params.craterWidthMax === "number") {
      const clampedCraterWidthMax = Math.max(1, Math.min(200000, action.params.craterWidthMax));
      if (clampedCraterWidthMax !== action.params.craterWidthMax) {
        action.params.craterWidthMax = clampedCraterWidthMax;
      }
    }

    if (
      typeof action.params.craterWidthMin === "number" &&
      typeof action.params.craterWidthMax === "number" &&
      action.params.craterWidthMin > action.params.craterWidthMax
    ) {
      const swap = action.params.craterWidthMin;
      action.params.craterWidthMin = action.params.craterWidthMax;
      action.params.craterWidthMax = swap;
    }

    if (!action.params.useFullArea && (!action.params.center || !action.params.size)) {
      action.params.useFullArea = true;
      action.params.center = undefined;
      action.params.size = undefined;
    }

    const decision = requireApproval(action, "Policy: landscape generation always requires approval.", policy, "medium");
    approved = decision.approved;
    risk = decision.risk;
    message = decision.message;
  }

  if (action.command === "scene.deleteActor") {
    if (action.params.target === "selection") {
      return hardDeny(
        action,
        "Policy hard-deny: scene.deleteActor target=selection is blocked. Use target=byName with explicit actorNames.",
        policy
      );
    }
    if ((action.params.actorNames?.length ?? 0) > policy.maxDeleteByNameCount) {
      return hardDeny(
        action,
        `Policy hard-deny: scene.deleteActor byName supports up to ${policy.maxDeleteByNameCount} actors.`,
        policy
      );
    }
    const decision = requireApproval(action, "Policy: delete actions always require approval.", policy, "high");
    approved = decision.approved;
    risk = decision.risk;
    message = decision.message;
  }

  if (
    action.command === "scene.modifyActor" ||
    action.command === "scene.modifyComponent" ||
    action.command === "scene.setComponentMaterial" ||
    action.command === "scene.setComponentStaticMesh" ||
    action.command === "scene.addActorTag" ||
    action.command === "scene.setActorFolder" ||
    action.command === "scene.addActorLabelPrefix" ||
    action.command === "scene.duplicateActors" ||
    action.command === "scene.deleteActor" ||
    action.command === "scene.setDirectionalLightIntensity" ||
    action.command === "scene.setFogDensity" ||
    action.command === "scene.setPostProcessExposureCompensation" ||
    action.command === "landscape.sculpt" ||
    action.command === "landscape.paintLayer" ||
    action.command === "landscape.generate"
  ) {
    if (action.params.target === "byName" && action.params.actorNames) {
      if (action.params.actorNames.length > policy.maxTargetNames) {
        action.params.actorNames = action.params.actorNames.slice(0, policy.maxTargetNames);
        const decision = requireApproval(
          action,
          `Policy: actorNames capped to ${action.params.actorNames.length}.`,
          policy,
          "medium"
        );
        approved = decision.approved;
        risk = decision.risk;
        message = decision.message;
      }
    }
  }

  if (action.command === "scene.setComponentMaterial") {
    if (!isAllowedAssetPath(action.params.materialPath)) {
      const decision = requireApproval(
        action,
        "Policy: materialPath must start with /Game/ or /Engine/.",
        policy,
        "high"
      );
      approved = decision.approved;
      risk = decision.risk;
      message = decision.message;
    }
  }

  if (action.command === "scene.setComponentStaticMesh") {
    if (!isAllowedAssetPath(action.params.meshPath)) {
      const decision = requireApproval(
        action,
        "Policy: meshPath must start with /Game/ or /Engine/.",
        policy,
        "high"
      );
      approved = decision.approved;
      risk = decision.risk;
      message = decision.message;
    }
  }

  return {
    approved,
    risk,
    message,
    hardDenied: false,
    estimatedChanges: estimateActionChanges(action, policy)
  };
}

export function buildSessionActions(actions: PlanAction[], policy: PolicyRuntimeConfig): SessionAction[] {
  return buildSessionActionsForMode(actions, policy, "agent");
}

export function buildSessionActionsForMode(
  actions: PlanAction[],
  policy: PolicyRuntimeConfig,
  mode: "chat" | "agent"
): SessionAction[] {
  let consumedChangeUnits = 0;

  return actions.map((action) => {
    const decision = applyLocalPolicy(action, policy, mode);
    let state: SessionAction["state"] = "pending";
    let approved = decision.approved;
    let lastMessage = decision.message;

    if (decision.hardDenied) {
      state = "failed";
    } else {
      const nextConsumedChangeUnits = consumedChangeUnits + decision.estimatedChanges;
      if (nextConsumedChangeUnits > policy.maxSessionChangeUnits) {
        state = "failed";
        approved = false;
        lastMessage =
          `Policy hard-deny: session change budget exceeded (` +
          `${nextConsumedChangeUnits} > ${policy.maxSessionChangeUnits} units).`;
      } else {
        consumedChangeUnits = nextConsumedChangeUnits;
      }
    }

    return {
      action: {
        ...action,
        risk: decision.risk
      },
      approved,
      state,
      attempts: 0,
      lastMessage
    };
  });
}
