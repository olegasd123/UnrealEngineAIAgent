import type { SessionResult } from "../contracts.js";
import type { SessionData, SessionDecision } from "../sessions/sessionTypes.js";

export function applySessionResult(session: SessionData, result: SessionResult): void {
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

export function makeSessionDecision(session: SessionData): SessionDecision {
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
