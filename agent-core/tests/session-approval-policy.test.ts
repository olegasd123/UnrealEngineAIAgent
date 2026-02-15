import test from "node:test";
import assert from "node:assert/strict";

import type { PlanAction, PlanOutput, SessionStartRequest } from "../src/contracts.js";
import { SessionStore } from "../src/sessions/sessionStore.js";

function makeAction(risk: "low" | "medium" | "high"): PlanAction {
  return {
    command: "scene.modifyActor",
    params: {
      target: "selection",
      deltaLocation: { x: 1, y: 0, z: 0 }
    },
    risk
  };
}

function makePlan(risks: Array<"low" | "medium" | "high">): PlanOutput {
  return {
    summary: "Test plan",
    steps: risks.map((risk, index) => `Action ${index + 1}: ${risk}`),
    actions: risks.map((risk) => makeAction(risk)),
    goal: {
      id: "goal_primary",
      description: "Execute test actions.",
      priority: "medium"
    },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }]
  };
}

function makeStartRequest(mode: "chat" | "agent"): SessionStartRequest {
  return {
    prompt: "Test prompt",
    mode,
    context: {},
    maxRetries: 2
  };
}

test("Agent mode: low + low + low -> auto apply", () => {
  const store = new SessionStore();
  const decision0 = store.create(makeStartRequest("agent"), makePlan(["low", "low", "low"]));
  assert.equal(decision0.status, "ready_to_execute");
  assert.equal(decision0.nextActionIndex, 0);
  assert.equal(decision0.nextActionApproved, true);

  const decision1 = store.next(decision0.sessionId, { actionIndex: 0, ok: true, message: "ok 1" });
  assert.equal(decision1.status, "ready_to_execute");
  assert.equal(decision1.nextActionIndex, 1);
  assert.equal(decision1.nextActionApproved, true);

  const decision2 = store.next(decision0.sessionId, { actionIndex: 1, ok: true, message: "ok 2" });
  assert.equal(decision2.status, "ready_to_execute");
  assert.equal(decision2.nextActionIndex, 2);
  assert.equal(decision2.nextActionApproved, true);

  const decision3 = store.next(decision0.sessionId, { actionIndex: 2, ok: true, message: "ok 3" });
  assert.equal(decision3.status, "completed");
  assert.match(decision3.message, /Last result: ok 3/);
});

test("Agent mode: low + medium + low -> confirm at medium", () => {
  const store = new SessionStore();
  const decision0 = store.create(makeStartRequest("agent"), makePlan(["low", "medium", "low"]));
  assert.equal(decision0.status, "ready_to_execute");
  assert.equal(decision0.nextActionIndex, 0);
  assert.equal(decision0.nextActionApproved, true);

  const decision1 = store.next(decision0.sessionId, { actionIndex: 0, ok: true, message: "ok 1" });
  assert.equal(decision1.status, "awaiting_approval");
  assert.equal(decision1.nextActionIndex, 1);
  assert.equal(decision1.nextActionApproved, false);
});

test("Agent mode: low + low + high -> confirm at high", () => {
  const store = new SessionStore();
  const decision0 = store.create(makeStartRequest("agent"), makePlan(["low", "low", "high"]));
  assert.equal(decision0.status, "ready_to_execute");
  assert.equal(decision0.nextActionIndex, 0);
  assert.equal(decision0.nextActionApproved, true);

  const decision1 = store.next(decision0.sessionId, { actionIndex: 0, ok: true, message: "ok 1" });
  assert.equal(decision1.status, "ready_to_execute");
  assert.equal(decision1.nextActionIndex, 1);
  assert.equal(decision1.nextActionApproved, true);

  const decision2 = store.next(decision0.sessionId, { actionIndex: 1, ok: true, message: "ok 2" });
  assert.equal(decision2.status, "awaiting_approval");
  assert.equal(decision2.nextActionIndex, 2);
  assert.equal(decision2.nextActionApproved, false);
});

test("Chat mode: any risk requires confirmation", () => {
  const risks: Array<"low" | "medium" | "high"> = ["low", "medium", "high"];
  for (const risk of risks) {
    const store = new SessionStore();
    const decision = store.create(makeStartRequest("chat"), makePlan([risk]));
    assert.equal(decision.status, "awaiting_approval");
    assert.equal(decision.nextActionIndex, 0);
    assert.equal(decision.nextActionApproved, false);
  }
});

test("Agent mode: high first action requires confirmation immediately", () => {
  const store = new SessionStore();
  const decision = store.create(makeStartRequest("agent"), makePlan(["high", "low"]));
  assert.equal(decision.status, "awaiting_approval");
  assert.equal(decision.nextActionIndex, 0);
  assert.equal(decision.nextActionApproved, false);
});
