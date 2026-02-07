import type { PlanAction, PlanOutput, SessionResult, TaskRequest } from "../contracts.js";

export type ActionState = "pending" | "succeeded" | "failed";
export type SessionStatus = "ready_to_execute" | "awaiting_approval" | "completed" | "failed";

export interface SessionAction {
  action: PlanAction;
  approved: boolean;
  state: ActionState;
  attempts: number;
  lastMessage?: string;
}

export interface SessionData {
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

export interface LocalPolicyDecision {
  approved: boolean;
  risk: PlanAction["risk"];
  message?: string;
  hardDenied: boolean;
  estimatedChanges: number;
}

export interface SessionPolicyInput {
  actions: PlanAction[];
  maxRetries: number;
}

export interface SessionTransitionInput {
  session: SessionData;
  result: SessionResult;
}
