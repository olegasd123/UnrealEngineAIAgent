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

function riskRank(risk: "low" | "medium" | "high"): number {
  if (risk === "high") {
    return 2;
  }
  if (risk === "medium") {
    return 1;
  }
  return 0;
}

function hasUserDeniedAction(session: SessionData): boolean {
  return session.actions.some((action) => action.state === "failed" && action.lastMessage === "Rejected by user.");
}

function evaluateChecks(session: SessionData): SessionData["plan"]["checks"] {
  const hasFailedAction = session.actions.some((action) => action.state === "failed");
  const hasPendingAction = session.actions.some((action) => action.state === "pending");
  const hasHighRiskFailed = session.actions.some((action) => action.action.risk === "high" && action.state === "failed");
  const hasHighRiskPendingUnapproved = session.actions.some(
    (action) => action.action.risk === "high" && action.state === "pending" && !action.approved
  );

  return session.plan.checks.map((check) => {
    if (check.type === "constraint") {
      if (hasFailedAction) {
        return { ...check, status: "failed" };
      }
      return { ...check, status: "passed" };
    }

    if (check.type === "success") {
      if (hasFailedAction) {
        return { ...check, status: "failed" };
      }
      if (!hasPendingAction) {
        return { ...check, status: "passed" };
      }
      return { ...check, status: "pending" };
    }

    if (hasHighRiskFailed) {
      return { ...check, status: "failed" };
    }
    if (hasHighRiskPendingUnapproved) {
      return { ...check, status: "pending" };
    }
    return { ...check, status: "passed" };
  });
}

function resolveStopCondition(session: SessionData, checks: SessionData["plan"]["checks"]): SessionData["plan"]["stopConditions"][number] | undefined {
  const completedCount = session.actions.filter((action) => action.state === "succeeded").length;
  const pendingCount = session.actions.filter((action) => action.state === "pending").length;
  const totalAttempts = session.actions.reduce((sum, action) => sum + action.attempts, 0);

  for (const condition of session.plan.stopConditions) {
    if (condition.type === "all_checks_passed") {
      const hasChecks = checks.length > 0;
      const allPassed = hasChecks && checks.every((check) => check.status === "passed");
      if (allPassed && pendingCount === 0) {
        return condition;
      }
      continue;
    }

    if (condition.type === "max_iterations") {
      if (pendingCount > 0 && totalAttempts >= condition.value) {
        return condition;
      }
      continue;
    }

    if (condition.type === "no_progress") {
      if (pendingCount > 0 && completedCount === 0 && totalAttempts >= condition.iterations) {
        return condition;
      }
      continue;
    }

    if (condition.type === "risk_threshold") {
      const threshold = riskRank(condition.maxRisk);
      const aboveThreshold = session.actions.find((action) => riskRank(action.action.risk) > threshold);
      if (aboveThreshold && !aboveThreshold.approved) {
        return condition;
      }
      continue;
    }

    if (condition.type === "user_denied") {
      if (hasUserDeniedAction(session)) {
        return condition;
      }
      continue;
    }

    if (condition.type === "manual_stop") {
      const requested = Boolean((session.input.context as { manualStop?: unknown }).manualStop);
      if (requested) {
        return condition;
      }
    }
  }

  return undefined;
}

function buildStoppedDecision(
  session: SessionData,
  checks: SessionData["plan"]["checks"],
  stopCondition: SessionData["plan"]["stopConditions"][number]
): SessionDecision {
  const completedCount = session.actions.filter((action) => action.state === "succeeded").length;
  const totalCount = session.actions.length;

  if (stopCondition.type === "all_checks_passed") {
    return {
      sessionId: session.id,
      status: "completed",
      summary: session.plan.summary,
      steps: session.plan.steps,
      checks,
      matchedStopCondition: stopCondition,
      message: `Stopped by stopCondition=all_checks_passed. Progress: ${completedCount}/${totalCount} actions completed.`
    };
  }

  if (stopCondition.type === "risk_threshold") {
    const threshold = riskRank(stopCondition.maxRisk);
    const index = session.actions.findIndex(
      (action) => riskRank(action.action.risk) > threshold && action.state === "pending" && !action.approved
    );
    if (index >= 0) {
      const action = session.actions[index];
      return {
        sessionId: session.id,
        status: "awaiting_approval",
        summary: session.plan.summary,
        steps: session.plan.steps,
        checks,
        matchedStopCondition: stopCondition,
        nextActionIndex: index,
        nextAction: action.action,
        nextActionState: action.state,
        nextActionAttempts: action.attempts,
        nextActionApproved: action.approved,
        message: [
          `Stopped by stopCondition=risk_threshold (maxRisk=${stopCondition.maxRisk}).`,
          `Action ${index + 1} needs explicit approval (risk=${action.action.risk}).`,
          `Progress: ${completedCount}/${totalCount} actions completed.`
        ].join(" ")
      };
    }
  }

  return {
    sessionId: session.id,
    status: "failed",
    summary: session.plan.summary,
    steps: session.plan.steps,
    checks,
    matchedStopCondition: stopCondition,
    message: `Stopped by stopCondition=${stopCondition.type}. Progress: ${completedCount}/${totalCount} actions completed.`
  };
}

export function makeSessionDecision(session: SessionData): SessionDecision {
  const checks = evaluateChecks(session);
  const stopCondition = resolveStopCondition(session, checks);
  if (stopCondition) {
    return buildStoppedDecision(session, checks, stopCondition);
  }

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
      checks,
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
      checks,
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
      checks,
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
    checks,
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
