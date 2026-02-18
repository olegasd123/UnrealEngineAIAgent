import test from "node:test";
import assert from "node:assert/strict";

import type { PolicyRuntimeConfig } from "../src/config.js";
import type { PlanAction } from "../src/contracts.js";
import { buildSessionActionsForMode } from "../src/executor/policyLayer.js";

function basePolicy(): PolicyRuntimeConfig {
  return {
    maxCreateCount: 50,
    maxDuplicateCount: 10,
    maxTargetNames: 50,
    maxDeleteByNameCount: 20,
    selectionTargetEstimate: 5,
    maxSessionChangeUnits: 200,
    maxLandscapeBrushSize: 1000,
    maxLandscapeBrushStrength: 0.4
  };
}

test("Landscape sculpt is clamped by policy and requires approval", () => {
  const action: PlanAction = {
    command: "landscape.sculpt",
    params: {
      target: "selection",
      center: { x: 0, y: 0 },
      size: { x: 5000, y: 2000 },
      strength: 0.9,
      falloff: 1.4,
      mode: "raise"
    },
    risk: "low"
  };

  const [sessionAction] = buildSessionActionsForMode([action], basePolicy(), "agent");
  assert.ok(sessionAction);
  if (!sessionAction) {
    return;
  }

  assert.equal(sessionAction.action.command, "landscape.sculpt");
  if (sessionAction.action.command !== "landscape.sculpt") {
    return;
  }

  assert.equal(sessionAction.approved, false);
  assert.equal(sessionAction.action.risk, "medium");
  assert.equal(sessionAction.action.params.size.x, 1000);
  assert.equal(sessionAction.action.params.size.y, 1000);
  assert.equal(sessionAction.action.params.strength, 0.4);
  assert.equal(sessionAction.action.params.falloff, 1);
});

test("Landscape paint requires approval even with safe values", () => {
  const action: PlanAction = {
    command: "landscape.paintLayer",
    params: {
      target: "selection",
      center: { x: 500, y: 500 },
      size: { x: 800, y: 800 },
      layerName: "Grass",
      strength: 0.2,
      falloff: 0.5,
      mode: "add"
    },
    risk: "medium"
  };

  const [sessionAction] = buildSessionActionsForMode([action], basePolicy(), "agent");
  assert.ok(sessionAction);
  if (!sessionAction) {
    return;
  }

  assert.equal(sessionAction.approved, false);
  assert.equal(sessionAction.state, "pending");
  assert.equal(sessionAction.action.risk, "medium");
});

test("Landscape generate requires approval and clamps limits", () => {
  const action: PlanAction = {
    command: "landscape.generate",
    params: {
      target: "selection",
      theme: "nature_island",
      useFullArea: false,
      maxHeight: 50000,
      mountainCount: 50,
      mountainWidthMin: 12000,
      mountainWidthMax: 500,
      craterCountMin: 1000,
      craterCountMax: 10,
      craterWidthMin: 5000,
      craterWidthMax: 100
    },
    risk: "low"
  };

  const [sessionAction] = buildSessionActionsForMode([action], basePolicy(), "agent");
  assert.ok(sessionAction);
  if (!sessionAction) {
    return;
  }

  assert.equal(sessionAction.approved, false);
  assert.equal(sessionAction.action.risk, "medium");
  if (sessionAction.action.command !== "landscape.generate") {
    return;
  }
  assert.equal(sessionAction.action.params.maxHeight, 10000);
  assert.equal(sessionAction.action.params.mountainCount, 8);
  assert.equal(sessionAction.action.params.mountainWidthMin, 500);
  assert.equal(sessionAction.action.params.mountainWidthMax, 12000);
  assert.equal(sessionAction.action.params.craterCountMin, 10);
  assert.equal(sessionAction.action.params.craterCountMax, 500);
  assert.equal(sessionAction.action.params.craterWidthMin, 100);
  assert.equal(sessionAction.action.params.craterWidthMax, 5000);
  assert.equal(sessionAction.action.params.useFullArea, true);
});
