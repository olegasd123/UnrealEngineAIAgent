import { randomUUID } from "node:crypto";

import type {
  PlanOutput,
  SessionApproveRequest,
  SessionNextRequest,
  SessionResult,
  SessionResumeRequest,
  SessionStartRequest,
  TaskRequest
} from "../contracts.js";
import type { PolicyRuntimeConfig } from "../config.js";
import { applySessionResult, makeSessionDecision } from "../executor/decisionLayer.js";
import { buildSessionActionsForMode } from "../executor/policyLayer.js";
import type { SessionData, SessionDecision } from "./sessionTypes.js";

const DEFAULT_POLICY: PolicyRuntimeConfig = {
  maxCreateCount: 50,
  maxDuplicateCount: 10,
  maxTargetNames: 50,
  maxDeleteByNameCount: 20,
  selectionTargetEstimate: 5,
  maxSessionChangeUnits: 120
};

export type { SessionDecision } from "./sessionTypes.js";

export class SessionStore {
  constructor(private readonly policy: PolicyRuntimeConfig = DEFAULT_POLICY) {}

  private readonly sessions = new Map<string, SessionData>();

  private resolveMaxIterations(plan: PlanOutput): number {
    const configured = plan.stopConditions.find((condition) => condition.type === "max_iterations");
    return configured?.type === "max_iterations" ? configured.value : 1;
  }

  private advanceIterationIfNeeded(session: SessionData): void {
    const nextPendingIndex = session.actions.findIndex((action) => action.state === "pending");
    if (nextPendingIndex < 0) {
      return;
    }

    const currentIterationEnd = session.iterationStartActionIndex + session.actionsPerIteration - 1;
    if (nextPendingIndex <= currentIterationEnd) {
      return;
    }

    if (session.currentIteration >= session.maxIterations) {
      return;
    }

    session.currentIteration += 1;
    session.iterationStartActionIndex = nextPendingIndex;
    session.checkpointPending = false;
    session.checkpointActionIndex = undefined;

    const checkpointAction = session.actions[nextPendingIndex];
    if (checkpointAction.state === "pending") {
      if (!checkpointAction.approved) {
        session.checkpointPending = true;
        session.checkpointActionIndex = nextPendingIndex;
        checkpointAction.lastMessage = `Iteration ${session.currentIteration} checkpoint: explicit approval required before continuing.`;
      }
    }
  }

  create(input: SessionStartRequest, plan: PlanOutput): SessionDecision {
    const id = randomUUID();
    const requestInput: TaskRequest = {
      prompt: input.prompt,
      mode: input.mode,
      context: input.context
    };

    const maxIterations = this.resolveMaxIterations(plan);
    const actionsPerIteration = Math.max(1, Math.ceil(Math.max(1, plan.actions.length) / maxIterations));
    const session: SessionData = {
      id,
      createdAt: new Date().toISOString(),
      input: requestInput,
      plan,
      maxRetries: input.maxRetries,
      actions: buildSessionActionsForMode(plan.actions, this.policy, input.mode),
      currentIteration: 1,
      maxIterations,
      actionsPerIteration,
      iterationStartActionIndex: 0,
      checkpointPending: false
    };

    this.advanceIterationIfNeeded(session);
    this.sessions.set(id, session);
    return makeSessionDecision(session);
  }

  next(sessionId: string, result?: SessionResult): SessionDecision {
    const session = this.get(sessionId);
    if (result) {
      applySessionResult(session, result);
      this.advanceIterationIfNeeded(session);
    }
    return makeSessionDecision(session);
  }

  nextRequest(input: SessionNextRequest): SessionDecision {
    return this.next(input.sessionId, input.result);
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
      if (session.checkpointActionIndex === actionIndex) {
        session.checkpointPending = false;
        session.checkpointActionIndex = undefined;
      }
    } else if (session.checkpointActionIndex === actionIndex) {
      session.checkpointPending = false;
      session.checkpointActionIndex = undefined;
    }

    return makeSessionDecision(session);
  }

  approveRequest(input: SessionApproveRequest): SessionDecision {
    return this.approve(input.sessionId, input.actionIndex, input.approved);
  }

  resume(sessionId: string): SessionDecision {
    const session = this.get(sessionId);
    this.advanceIterationIfNeeded(session);
    return makeSessionDecision(session);
  }

  resumeRequest(input: SessionResumeRequest): SessionDecision {
    return this.resume(input.sessionId);
  }

  private get(sessionId: string): SessionData {
    const session = this.sessions.get(sessionId);
    if (!session) {
      throw new Error(`Session ${sessionId} was not found.`);
    }
    return session;
  }
}
