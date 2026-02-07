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
  currentIteration: number;
  maxIterations: number;
  actionsPerIteration: number;
  iterationStartActionIndex: number;
  checkpointPending: boolean;
  checkpointActionIndex?: number;
}

export interface SessionDecision {
  sessionId: string;
  status: SessionStatus;
  summary: string;
  steps: string[];
  iteration: {
    current: number;
    max: number;
    actionsPerIteration: number;
    checkpointPending: boolean;
  };
  checks?: PlanOutput["checks"];
  matchedStopCondition?: PlanOutput["stopConditions"][number];
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
