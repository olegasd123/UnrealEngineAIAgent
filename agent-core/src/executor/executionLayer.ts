import type {
  PlanOutput,
  SessionApproveRequest,
  SessionNextRequest,
  SessionResumeRequest,
  SessionStartRequest
} from "../contracts.js";
import type { SessionDecision } from "../sessions/sessionStore.js";
import { SessionStore } from "../sessions/sessionStore.js";

export class ExecutionLayer {
  constructor(private readonly sessionStore: SessionStore) {}

  startSession(input: SessionStartRequest, plan: PlanOutput): SessionDecision {
    return this.sessionStore.create(input, plan);
  }

  next(input: SessionNextRequest): SessionDecision {
    return this.sessionStore.next(input.sessionId, input.result);
  }

  approve(input: SessionApproveRequest): SessionDecision {
    return this.sessionStore.approve(input.sessionId, input.actionIndex, input.approved);
  }

  resume(input: SessionResumeRequest): SessionDecision {
    return this.sessionStore.resume(input.sessionId);
  }
}
