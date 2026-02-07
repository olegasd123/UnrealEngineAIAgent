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
import { buildSessionActions } from "../executor/policyLayer.js";
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

  create(input: SessionStartRequest, plan: PlanOutput): SessionDecision {
    const id = randomUUID();
    const requestInput: TaskRequest = {
      prompt: input.prompt,
      mode: input.mode,
      context: input.context
    };

    const session: SessionData = {
      id,
      createdAt: new Date().toISOString(),
      input: requestInput,
      plan,
      maxRetries: input.maxRetries,
      actions: buildSessionActions(plan.actions, this.policy)
    };

    this.sessions.set(id, session);
    return makeSessionDecision(session);
  }

  next(sessionId: string, result?: SessionResult): SessionDecision {
    const session = this.get(sessionId);
    if (result) {
      applySessionResult(session, result);
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
    }

    return makeSessionDecision(session);
  }

  approveRequest(input: SessionApproveRequest): SessionDecision {
    return this.approve(input.sessionId, input.actionIndex, input.approved);
  }

  resume(sessionId: string): SessionDecision {
    const session = this.get(sessionId);
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
