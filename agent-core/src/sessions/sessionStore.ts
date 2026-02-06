import { randomUUID } from "node:crypto";

import type {
  PlanAction,
  PlanOutput,
  SessionResult,
  SessionStartRequest,
  TaskRequest
} from "../contracts.js";

type ActionState = "pending" | "succeeded" | "failed";
type SessionStatus = "ready_to_execute" | "awaiting_approval" | "completed" | "failed";

const MAX_CREATE_COUNT = 50;
const MAX_DUPLICATE_COUNT = 10;
const MAX_TARGET_NAMES = 50;
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

interface SessionAction {
  action: PlanAction;
  approved: boolean;
  state: ActionState;
  attempts: number;
  lastMessage?: string;
}

interface SessionData {
  id: string;
  createdAt: string;
  input: TaskRequest;
  plan: PlanOutput;
  maxRetries: number;
  actions: SessionAction[];
}

export interface SessionDecision {
  sessionId: string;
  status: SessionStatus;
  summary: string;
  steps: string[];
  nextActionIndex?: number;
  nextAction?: PlanAction;
  nextActionState?: ActionState;
  nextActionAttempts?: number;
  nextActionApproved?: boolean;
  message: string;
}

function shouldAutoApprove(risk: PlanAction["risk"]): boolean {
  return risk === "low";
}

function normalizeAssetPath(path: string): string {
  return path.trim();
}

function isAllowedAssetPath(path: string): boolean {
  const normalized = normalizeAssetPath(path);
  return normalized.startsWith("/Game/") || normalized.startsWith("/Engine/");
}

function requireApproval(
  action: PlanAction,
  reason: string,
  riskOverride: PlanAction["risk"] = "high"
): { approved: boolean; risk: PlanAction["risk"]; message: string } {
  return {
    approved: false,
    risk: riskOverride,
    message: reason
  };
}

function applyLocalPolicy(action: PlanAction): { approved: boolean; risk: PlanAction["risk"]; message?: string } {
  let risk: PlanAction["risk"] = action.risk;
  let approved = shouldAutoApprove(risk);
  let message: string | undefined;

  if (action.command === "scene.createActor") {
    const actorClass = action.params.actorClass;
    if (!ALLOWED_CREATE_ACTOR_CLASSES.has(actorClass)) {
      return requireApproval(
        action,
        `Policy: actorClass '${actorClass}' is not in the allowlist.`,
        "high"
      );
    }

    if (action.params.count > MAX_CREATE_COUNT) {
      action.params.count = MAX_CREATE_COUNT;
      const policy = requireApproval(
        action,
        `Policy: create count capped to ${MAX_CREATE_COUNT}.`,
        "medium"
      );
      approved = policy.approved;
      risk = policy.risk;
      message = policy.message;
    }
  }

  if (action.command === "scene.duplicateActors") {
    if (action.params.count > MAX_DUPLICATE_COUNT) {
      action.params.count = MAX_DUPLICATE_COUNT;
      const policy = requireApproval(
        action,
        `Policy: duplicate count capped to ${MAX_DUPLICATE_COUNT}.`,
        "medium"
      );
      approved = policy.approved;
      risk = policy.risk;
      message = policy.message;
    }
  }

  if (action.command === "scene.deleteActor") {
    const policy = requireApproval(action, "Policy: delete actions always require approval.", "high");
    approved = policy.approved;
    risk = policy.risk;
    message = policy.message;
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
    action.command === "scene.deleteActor"
  ) {
    if (action.params.target === "byName" && action.params.actorNames) {
      if (action.params.actorNames.length > MAX_TARGET_NAMES) {
        action.params.actorNames = action.params.actorNames.slice(0, MAX_TARGET_NAMES);
        const policy = requireApproval(
          action,
          `Policy: actorNames capped to ${MAX_TARGET_NAMES}.`,
          "medium"
        );
        approved = policy.approved;
        risk = policy.risk;
        message = policy.message;
      }
    }
  }

  if (action.command === "scene.setComponentMaterial") {
    if (!isAllowedAssetPath(action.params.materialPath)) {
      const policy = requireApproval(
        action,
        "Policy: materialPath must start with /Game/ or /Engine/.",
        "high"
      );
      approved = policy.approved;
      risk = policy.risk;
      message = policy.message;
    }
  }

  if (action.command === "scene.setComponentStaticMesh") {
    if (!isAllowedAssetPath(action.params.meshPath)) {
      const policy = requireApproval(
        action,
        "Policy: meshPath must start with /Game/ or /Engine/.",
        "high"
      );
      approved = policy.approved;
      risk = policy.risk;
      message = policy.message;
    }
  }

  return { approved, risk, message };
}

export class SessionStore {
  private readonly sessions = new Map<string, SessionData>();

  create(input: SessionStartRequest, plan: PlanOutput): SessionDecision {
    const id = randomUUID();
    const session: SessionData = {
      id,
      createdAt: new Date().toISOString(),
      input: {
        prompt: input.prompt,
        mode: input.mode,
        context: input.context
      },
      plan,
      maxRetries: input.maxRetries,
      actions: plan.actions.map((action) => {
        const policy = applyLocalPolicy(action);
        return {
          action: {
            ...action,
            risk: policy.risk
          },
          approved: policy.approved,
          state: "pending",
          attempts: 0,
          lastMessage: policy.message
        };
      })
    };
    this.sessions.set(id, session);
    return this.makeDecision(session);
  }

  next(sessionId: string, result?: SessionResult): SessionDecision {
    const session = this.get(sessionId);
    if (result) {
      this.applyResult(session, result);
    }
    return this.makeDecision(session);
  }

  approve(sessionId: string, actionIndex: number, approved: boolean): SessionDecision {
    const session = this.get(sessionId);
    const action = session.actions[actionIndex];
    if (!action) {
      throw new Error(`Action index ${actionIndex} is out of range.`);
    }
    if (action.state !== "pending") {
      throw new Error(`Action ${actionIndex} is already ${action.state}.`);
    }
    action.approved = approved;
    if (!approved) {
      action.state = "failed";
      action.lastMessage = "Rejected by user.";
    }
    return this.makeDecision(session);
  }

  resume(sessionId: string): SessionDecision {
    const session = this.get(sessionId);
    return this.makeDecision(session);
  }

  private get(sessionId: string): SessionData {
    const session = this.sessions.get(sessionId);
    if (!session) {
      throw new Error(`Session ${sessionId} was not found.`);
    }
    return session;
  }

  private applyResult(session: SessionData, result: SessionResult): void {
    const action = session.actions[result.actionIndex];
    if (!action) {
      throw new Error(`Action index ${result.actionIndex} is out of range.`);
    }
    if (action.state !== "pending") {
      throw new Error(`Action ${result.actionIndex} is already ${action.state}.`);
    }

    action.attempts += 1;
    action.lastMessage = result.message;

    if (result.ok) {
      action.state = "succeeded";
      return;
    }

    if (action.attempts >= session.maxRetries + 1) {
      action.state = "failed";
    }
  }

  private makeDecision(session: SessionData): SessionDecision {
    const completedCount = session.actions.filter((action) => action.state === "succeeded").length;
    const totalCount = session.actions.length;

    const failedActionIndex = session.actions.findIndex((action) => action.state === "failed");
    if (failedActionIndex >= 0) {
      const failed = session.actions[failedActionIndex];
      return {
        sessionId: session.id,
        status: "failed",
        summary: session.plan.summary,
        steps: session.plan.steps,
        nextActionIndex: failedActionIndex,
        nextAction: failed.action,
        nextActionState: failed.state,
        nextActionAttempts: failed.attempts,
        nextActionApproved: failed.approved,
        message: [
          `Action ${failedActionIndex + 1} failed after ${failed.attempts} attempt(s).`,
          failed.lastMessage ? `Last error: ${failed.lastMessage}` : null,
          `Progress: ${completedCount}/${totalCount} actions completed.`
        ]
          .filter(Boolean)
          .join(" ")
      };
    }

    const pendingActionIndex = session.actions.findIndex((action) => action.state === "pending");
    if (pendingActionIndex < 0) {
      return {
        sessionId: session.id,
        status: "completed",
        summary: session.plan.summary,
        steps: session.plan.steps,
        message: `All actions are completed (${completedCount}/${totalCount}).`
      };
    }

    const pending = session.actions[pendingActionIndex];
    if (!pending.approved) {
      return {
        sessionId: session.id,
        status: "awaiting_approval",
        summary: session.plan.summary,
        steps: session.plan.steps,
        nextActionIndex: pendingActionIndex,
        nextAction: pending.action,
        nextActionState: pending.state,
        nextActionAttempts: pending.attempts,
        nextActionApproved: pending.approved,
        message: [
          `Action ${pendingActionIndex + 1} is waiting for approval (risk=${pending.action.risk}).`,
          pending.lastMessage ? `Last result: ${pending.lastMessage}` : null,
          `Progress: ${completedCount}/${totalCount} actions completed.`
        ]
          .filter(Boolean)
          .join(" ")
      };
    }

    return {
      sessionId: session.id,
      status: "ready_to_execute",
      summary: session.plan.summary,
      steps: session.plan.steps,
      nextActionIndex: pendingActionIndex,
      nextAction: pending.action,
      nextActionState: pending.state,
      nextActionAttempts: pending.attempts,
      nextActionApproved: pending.approved,
      message: [
        `Action ${pendingActionIndex + 1} is ready to execute (attempt ${pending.attempts + 1}/${session.maxRetries + 1}).`,
        pending.lastMessage ? `Last result: ${pending.lastMessage}` : null,
        `Progress: ${completedCount}/${totalCount} actions completed.`
      ]
        .filter(Boolean)
        .join(" ")
    };
  }
}
