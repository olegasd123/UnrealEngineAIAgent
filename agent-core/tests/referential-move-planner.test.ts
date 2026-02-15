import assert from "node:assert/strict";
import test from "node:test";

import { buildRuleBasedPlan } from "../src/planner/ruleBasedPlanner.js";
import { IntentLayer } from "../src/intent/intentLayer.js";
import { ValidationLayer } from "../src/validator/validationLayer.js";
import type { TaskRequest } from "../src/contracts.js";

test("Rule planner parses 'move it back to 100' and uses remembered selection byName", () => {
  const request: TaskRequest = {
    prompt: "move it back to 100",
    mode: "chat",
    context: {
      selectionNames: ["MyActor"]
    }
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.summary, "Move selected actor back 100 units along X.");
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.modifyActor");
  if (action.command !== "scene.modifyActor") {
    return;
  }
  assert.equal(action.params.target, "byName");
  assert.deepEqual(action.params.actorNames, ["MyActor"]);
  assert.deepEqual(action.params.deltaLocation, { x: -100, y: 0, z: 0 });
});

test("Validation rewrites selection target to byName when context has remembered selection", () => {
  const request: TaskRequest = {
    prompt: "move it on x 10",
    mode: "chat",
    context: {
      selectionNames: ["ActorFromMemory"]
    }
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move",
    steps: ["step"],
    actions: [
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "low"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  const action = validated.plan.actions[0];
  assert.equal(action.command, "scene.modifyActor");
  if (action.command !== "scene.modifyActor") {
    return;
  }
  assert.equal(action.params.target, "byName");
  assert.deepEqual(action.params.actorNames, ["ActorFromMemory"]);
});

test("Validation normalizes safe action risk to low", () => {
  const request: TaskRequest = {
    prompt: "move it on x 10",
    mode: "agent",
    context: {
      selectionNames: ["ActorFromMemory"]
    }
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move",
    steps: ["step"],
    actions: [
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "medium"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.risk, "low");
});

test("Rule planner builds context.getSelection for selection info request", () => {
  const request: TaskRequest = {
    prompt: "what is selected right now?",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.summary, "Collect current selection context.");
  const action = plan.actions[0];
  assert.equal(action.command, "context.getSelection");
  if (action.command !== "context.getSelection") {
    return;
  }
  assert.deepEqual(action.params, {});
  assert.equal(action.risk, "low");
});

test("Rule planner builds context.getSceneSummary for scene summary request", () => {
  const request: TaskRequest = {
    prompt: "show scene summary",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  assert.equal(plan.summary, "Collect current scene summary context.");
  const action = plan.actions[0];
  assert.equal(action.command, "context.getSceneSummary");
  if (action.command !== "context.getSceneSummary") {
    return;
  }
  assert.deepEqual(action.params, {});
  assert.equal(action.risk, "low");
});

test("Validation normalizes context action risk to low", () => {
  const request: TaskRequest = {
    prompt: "show selection info",
    mode: "agent",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "selection info",
    steps: ["step"],
    actions: [{ command: "context.getSelection", params: {}, risk: "medium" }],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.risk, "low");
});

test("Validation normalizes context-only summary text", () => {
  const request: TaskRequest = {
    prompt: "what is selected right now?",
    mode: "agent",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "Retrieve current selection context.",
    steps: ["step"],
    actions: [{ command: "context.getSelection", params: {}, risk: "low" }],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.summary, "Collect current selection context.");
});

test("Validation adds internal transaction for multi-action agent plan", () => {
  const request: TaskRequest = {
    prompt: "move and rotate selected actor",
    mode: "agent",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move and rotate",
    steps: ["step"],
    actions: [
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "low"
      },
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaRotation: { pitch: 0, yaw: 15, roll: 0 } },
        risk: "low"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.command, "session.beginTransaction");
  assert.equal(validated.plan.actions[validated.plan.actions.length - 1]?.command, "session.commitTransaction");
});

test("Validation does not add internal transaction for multi-action chat plan", () => {
  const request: TaskRequest = {
    prompt: "move and rotate selected actor",
    mode: "chat",
    context: {}
  };
  const intent = new IntentLayer().normalize(request);
  const validator = new ValidationLayer();

  const candidate = {
    summary: "move and rotate",
    steps: ["step"],
    actions: [
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaLocation: { x: 10, y: 0, z: 0 } },
        risk: "low"
      },
      {
        command: "scene.modifyActor",
        params: { target: "selection", deltaRotation: { pitch: 0, yaw: 15, roll: 0 } },
        risk: "low"
      }
    ],
    goal: { id: "g1", description: "d", priority: "medium" },
    subgoals: [],
    checks: [],
    stopConditions: [{ type: "all_checks_passed" }]
  };

  const validated = validator.validatePlan(intent, candidate);
  assert.equal(validated.plan.actions[0]?.command, "scene.modifyActor");
  assert.equal(validated.plan.actions[validated.plan.actions.length - 1]?.command, "scene.modifyActor");
});

test("Rule planner parses directional light intensity action", () => {
  const request: TaskRequest = {
    prompt: "set directional light intensity to 8.5",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.setDirectionalLightIntensity");
  if (action.command !== "scene.setDirectionalLightIntensity") {
    return;
  }
  assert.equal(action.params.intensity, 8.5);
});

test("Rule planner parses fog density action", () => {
  const request: TaskRequest = {
    prompt: "set fog density to 0.03",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.setFogDensity");
  if (action.command !== "scene.setFogDensity") {
    return;
  }
  assert.equal(action.params.density, 0.03);
});

test("Rule planner parses exposure compensation action", () => {
  const request: TaskRequest = {
    prompt: "set exposure compensation to -1.2",
    mode: "chat",
    context: {}
  };

  const plan = buildRuleBasedPlan(request);
  assert.equal(plan.actions.length, 1);
  const action = plan.actions[0];
  assert.equal(action.command, "scene.setPostProcessExposureCompensation");
  if (action.command !== "scene.setPostProcessExposureCompensation") {
    return;
  }
  assert.equal(action.params.exposureCompensation, -1.2);
});
