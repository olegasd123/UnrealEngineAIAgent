/*
 * Generated from shared/schemas/ue-tool-command.schema.json.
 * Do not edit manually.
 */

import { z } from "zod";

export const AllowedCommands = [
  "context.getSceneSummary",
  "context.getSelection",
  "scene.setDirectionalLightIntensity",
  "scene.setFogDensity",
  "scene.setPostProcessExposureCompensation",
  "scene.createActor",
  "scene.modifyActor",
  "scene.deleteActor",
  "scene.modifyComponent",
  "scene.setComponentMaterial",
  "scene.setComponentStaticMesh",
  "scene.addActorTag",
  "scene.setActorFolder",
  "scene.addActorLabelPrefix",
  "scene.duplicateActors",
  "landscape.sculpt",
  "landscape.paintLayer",
  "editor.undo",
  "editor.redo",
  "session.beginTransaction",
  "session.commitTransaction",
  "session.rollbackTransaction",
] as const;

export type AllowedCommand = (typeof AllowedCommands)[number];

export const UeToolCommandSchema = z
  .object({
    command: z.enum(AllowedCommands),
    params: z.record(z.unknown())
  })
  .strict();

export type UeToolCommand = z.infer<typeof UeToolCommandSchema>;
