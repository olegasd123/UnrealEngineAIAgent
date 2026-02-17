import { z } from "zod";

export { AllowedCommands, UeToolCommandSchema, type UeToolCommand } from "./generated/ueToolCommandSchema.js";

const QualityTierSchema = z.enum(["low", "medium", "high", "cinematic"]);

const WorldStateEnvironmentContextSchema = z
  .object({
    mapName: z.string().trim().min(1).optional(),
    levelName: z.string().trim().min(1).optional(),
    gameStyle: z.string().trim().min(1).optional(),
    timeOfDay: z.string().trim().min(1).optional(),
    weather: z.string().trim().min(1).optional()
  })
  .strict();

const WorldStateLightingContextSchema = z
  .object({
    hasDirectionalLight: z.boolean().optional(),
    hasSkyLight: z.boolean().optional(),
    hasFog: z.boolean().optional(),
    exposureCompensation: z.number().finite().optional(),
    directionalLightIntensity: z.number().finite().optional(),
    skyLightIntensity: z.number().finite().optional(),
    fogDensity: z.number().finite().optional()
  })
  .strict();

const WorldStateMaterialsContextSchema = z
  .object({
    styleHint: z.string().trim().min(1).optional(),
    targetMaterialPaths: z.array(z.string().trim().min(1)).optional(),
    selectedMaterialCount: z.number().int().min(0).optional()
  })
  .strict();

const WorldStatePerformanceContextSchema = z
  .object({
    qualityTier: QualityTierSchema.optional(),
    targetFps: z.number().finite().optional(),
    maxDrawCalls: z.number().int().min(0).optional()
  })
  .strict();

const WorldStateAssetsContextSchema = z
  .object({
    materialPaths: z.array(z.string().trim().min(1)).optional(),
    meshPaths: z.array(z.string().trim().min(1)).optional()
  })
  .strict();

const SelectionActorSchema = z
  .object({
    name: z.string().trim().min(1),
    label: z.string().trim().min(1).optional(),
    class: z.string().trim().min(1).optional(),
    location: z
      .object({
        x: z.number().finite(),
        y: z.number().finite(),
        z: z.number().finite()
      })
      .strict()
      .optional(),
    rotation: z
      .object({
        pitch: z.number().finite(),
        yaw: z.number().finite(),
        roll: z.number().finite()
      })
      .strict()
      .optional(),
    scale: z
      .object({
        x: z.number().finite(),
        y: z.number().finite(),
        z: z.number().finite()
      })
      .strict()
      .optional(),
    components: z
      .array(
        z
          .object({
            name: z.string().trim().min(1),
            class: z.string().trim().min(1).optional()
          })
          .strict()
      )
      .optional()
  })
  .strict();

const LevelContextSchema = z
  .object({
    mapName: z.string().trim().min(1).optional(),
    levelName: z.string().trim().min(1).optional()
  })
  .strict();

export const TaskContextSchema = z
  .object({
    selection: z.array(z.union([z.string().trim().min(1), SelectionActorSchema])).optional(),
    selectionNames: z.array(z.string().trim().min(1)).optional(),
    mapName: z.string().trim().min(1).optional(),
    levelName: z.string().trim().min(1).optional(),
    gameStyle: z.string().trim().min(1).optional(),
    timeOfDay: z.string().trim().min(1).optional(),
    weather: z.string().trim().min(1).optional(),
    materialStyle: z.string().trim().min(1).optional(),
    qualityTier: QualityTierSchema.optional(),
    targetFps: z.number().finite().optional(),
    maxDrawCalls: z.number().int().min(0).optional(),
    manualStop: z.boolean().optional(),
    environment: WorldStateEnvironmentContextSchema.optional(),
    lighting: WorldStateLightingContextSchema.optional(),
    materials: WorldStateMaterialsContextSchema.optional(),
    performance: WorldStatePerformanceContextSchema.optional(),
    assets: WorldStateAssetsContextSchema.optional()
    ,
    level: LevelContextSchema.optional()
  })
  .strict();

export const ProviderNameSchema = z.enum(["openai", "gemini", "local"]);

export const TaskRequestSchema = z.object({
  prompt: z.string().min(1),
  mode: z.enum(["chat", "agent"]).default("chat"),
  context: TaskContextSchema.default({}),
  provider: ProviderNameSchema.optional(),
  model: z.string().trim().min(1).optional(),
  chatId: z.string().uuid().optional()
});

export type TaskRequest = z.infer<typeof TaskRequestSchema>;

const DeltaLocationSchema = z.object({
  x: z.number(),
  y: z.number(),
  z: z.number()
});

const DeltaRotationSchema = z.object({
  pitch: z.number(),
  yaw: z.number(),
  roll: z.number()
});

const DeltaScaleSchema = z.object({
  x: z.number(),
  y: z.number(),
  z: z.number()
});

const ScaleSchema = z.object({
  x: z.number(),
  y: z.number(),
  z: z.number()
});

const SceneModifyActorParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    deltaLocation: DeltaLocationSchema.optional(),
    deltaRotation: DeltaRotationSchema.optional(),
    deltaScale: DeltaScaleSchema.optional(),
    scale: ScaleSchema.optional()
  })
  .refine((value) => Boolean(value.deltaLocation || value.deltaRotation || value.deltaScale || value.scale), {
    message: "scene.modifyActor action needs deltaLocation, deltaRotation, deltaScale, or scale"
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.modifyActor target=byName needs actorNames"
  });

export const PlanActionSchema = z.object({
  command: z.literal("scene.modifyActor"),
  params: SceneModifyActorParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

const SceneCreateActorParamsSchema = z.object({
  actorClass: z.string().min(1),
  location: DeltaLocationSchema.optional(),
  rotation: DeltaRotationSchema.optional(),
  count: z.number().int().min(1).max(200).default(1)
});

const SceneDeleteActorParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.deleteActor target=byName needs actorNames"
  });

const SceneModifyComponentParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    componentName: z.string().min(1),
    deltaLocation: DeltaLocationSchema.optional(),
    deltaRotation: DeltaRotationSchema.optional(),
    deltaScale: DeltaScaleSchema.optional(),
    scale: ScaleSchema.optional(),
    visibility: z.boolean().optional()
  })
  .refine((value) => Boolean(value.deltaLocation || value.deltaRotation || value.deltaScale || value.scale || value.visibility !== undefined), {
    message: "scene.modifyComponent action needs a transform or visibility"
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.modifyComponent target=byName needs actorNames"
  });

const SceneSetComponentMaterialParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    componentName: z.string().min(1),
    materialPath: z.string().min(1),
    materialSlot: z.number().int().min(0).max(32).optional()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.setComponentMaterial target=byName needs actorNames"
  });

const SceneSetComponentStaticMeshParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    componentName: z.string().min(1),
    meshPath: z.string().min(1)
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.setComponentStaticMesh target=byName needs actorNames"
  });

const SceneAddActorTagParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    tag: z.string().min(1)
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.addActorTag target=byName needs actorNames"
  });

const SceneSetActorFolderParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    folderPath: z.string()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.setActorFolder target=byName needs actorNames"
  });

const SceneAddActorLabelPrefixParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    prefix: z.string().min(1)
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.addActorLabelPrefix target=byName needs actorNames"
  });

const SceneDuplicateActorsParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    count: z.number().int().min(1).max(20).default(1),
    offset: DeltaLocationSchema.optional()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.duplicateActors target=byName needs actorNames"
  });

const SceneSetDirectionalLightIntensityParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    intensity: z.number().finite()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.setDirectionalLightIntensity target=byName needs actorNames"
  });

const SceneSetFogDensityParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    density: z.number().finite()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.setFogDensity target=byName needs actorNames"
  });

const SceneSetPostProcessExposureCompensationParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    exposureCompensation: z.number().finite()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.setPostProcessExposureCompensation target=byName needs actorNames"
  });

const LandscapePoint2Schema = z.object({
  x: z.number().finite(),
  y: z.number().finite()
});

const LandscapeSize2Schema = z.object({
  x: z.number().finite().positive(),
  y: z.number().finite().positive()
});

const LandscapeDetailLevelSchema = z.enum(["low", "medium", "high", "cinematic"]);
const LandscapeMoonProfileSchema = z.enum(["moon_surface"]);

const LandscapeSculptParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    center: LandscapePoint2Schema,
    size: LandscapeSize2Schema,
    strength: z.number().finite().min(0).max(1),
    falloff: z.number().finite().min(0).max(1).default(0.5),
    mode: z.enum(["raise", "lower"]).default("raise")
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "landscape.sculpt target=byName needs actorNames"
  });

const LandscapePaintLayerParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    center: LandscapePoint2Schema,
    size: LandscapeSize2Schema,
    layerName: z.string().min(1),
    strength: z.number().finite().min(0).max(1),
    falloff: z.number().finite().min(0).max(1).default(0.5),
    mode: z.enum(["add", "remove"]).default("add")
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "landscape.paintLayer target=byName needs actorNames"
  });

const LandscapeGenerateParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    theme: z.enum(["moon_surface", "nature_island"]),
    detailLevel: LandscapeDetailLevelSchema.optional(),
    moonProfile: LandscapeMoonProfileSchema.optional(),
    useFullArea: z.boolean().default(true),
    center: LandscapePoint2Schema.optional(),
    size: LandscapeSize2Schema.optional(),
    seed: z.number().int().optional(),
    mountainCount: z.number().int().min(1).max(8).optional(),
    mountainWidthMin: z.number().finite().positive().max(200000).optional(),
    mountainWidthMax: z.number().finite().positive().max(200000).optional(),
    maxHeight: z.number().finite().positive().max(20000).optional(),
    riverCountMin: z.number().int().min(0).max(32).optional(),
    riverCountMax: z.number().int().min(0).max(32).optional(),
    riverWidthMin: z.number().finite().positive().max(200000).optional(),
    riverWidthMax: z.number().finite().positive().max(200000).optional(),
    lakeCountMin: z.number().int().min(0).max(32).optional(),
    lakeCountMax: z.number().int().min(0).max(32).optional(),
    lakeWidthMin: z.number().finite().positive().max(200000).optional(),
    lakeWidthMax: z.number().finite().positive().max(200000).optional(),
    craterCountMin: z.number().int().min(1).max(500).optional(),
    craterCountMax: z.number().int().min(1).max(500).optional(),
    craterWidthMin: z.number().finite().positive().max(200000).optional(),
    craterWidthMax: z.number().finite().positive().max(200000).optional()
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "landscape.generate target=byName needs actorNames"
  })
  .refine((value) => (value.useFullArea ? true : Boolean(value.center && value.size)), {
    message: "landscape.generate with useFullArea=false needs center and size"
  })
  .superRefine((value, ctx) => {
    if (
      value.mountainWidthMin !== undefined &&
      value.mountainWidthMax !== undefined &&
      value.mountainWidthMin > value.mountainWidthMax
    ) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "landscape.generate mountainWidthMin must be <= mountainWidthMax"
      });
    }

    if (
      value.riverCountMin !== undefined &&
      value.riverCountMax !== undefined &&
      value.riverCountMin > value.riverCountMax
    ) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "landscape.generate riverCountMin must be <= riverCountMax"
      });
    }

    if (
      value.riverWidthMin !== undefined &&
      value.riverWidthMax !== undefined &&
      value.riverWidthMin > value.riverWidthMax
    ) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "landscape.generate riverWidthMin must be <= riverWidthMax"
      });
    }

    if (
      value.lakeCountMin !== undefined &&
      value.lakeCountMax !== undefined &&
      value.lakeCountMin > value.lakeCountMax
    ) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "landscape.generate lakeCountMin must be <= lakeCountMax"
      });
    }

    if (
      value.lakeWidthMin !== undefined &&
      value.lakeWidthMax !== undefined &&
      value.lakeWidthMin > value.lakeWidthMax
    ) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "landscape.generate lakeWidthMin must be <= lakeWidthMax"
      });
    }

    if (
      value.craterCountMin !== undefined &&
      value.craterCountMax !== undefined &&
      value.craterCountMin > value.craterCountMax
    ) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "landscape.generate craterCountMin must be <= craterCountMax"
      });
    }

    if (
      value.craterWidthMin !== undefined &&
      value.craterWidthMax !== undefined &&
      value.craterWidthMin > value.craterWidthMax
    ) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "landscape.generate craterWidthMin must be <= craterWidthMax"
      });
    }
  });

const ContextGetSceneSummaryParamsSchema = z.object({}).passthrough();

const ContextGetSelectionParamsSchema = z.object({}).passthrough();

const EditorUndoParamsSchema = z.object({}).passthrough();

const EditorRedoParamsSchema = z.object({}).passthrough();

const SessionBeginTransactionParamsSchema = z
  .object({
    description: z.string().min(1).optional()
  })
  .passthrough();

const SessionCommitTransactionParamsSchema = z.object({}).passthrough();

const SessionRollbackTransactionParamsSchema = z.object({}).passthrough();

export const ContextGetSceneSummaryActionSchema = z.object({
  command: z.literal("context.getSceneSummary"),
  params: ContextGetSceneSummaryParamsSchema.default({}),
  risk: z.enum(["low", "medium", "high"])
});

export const ContextGetSelectionActionSchema = z.object({
  command: z.literal("context.getSelection"),
  params: ContextGetSelectionParamsSchema.default({}),
  risk: z.enum(["low", "medium", "high"])
});

export const EditorUndoActionSchema = z.object({
  command: z.literal("editor.undo"),
  params: EditorUndoParamsSchema.default({}),
  risk: z.enum(["low", "medium", "high"])
});

export const EditorRedoActionSchema = z.object({
  command: z.literal("editor.redo"),
  params: EditorRedoParamsSchema.default({}),
  risk: z.enum(["low", "medium", "high"])
});

export const SessionBeginTransactionActionSchema = z.object({
  command: z.literal("session.beginTransaction"),
  params: SessionBeginTransactionParamsSchema.default({}),
  risk: z.enum(["low", "medium", "high"])
});

export const SessionCommitTransactionActionSchema = z.object({
  command: z.literal("session.commitTransaction"),
  params: SessionCommitTransactionParamsSchema.default({}),
  risk: z.enum(["low", "medium", "high"])
});

export const SessionRollbackTransactionActionSchema = z.object({
  command: z.literal("session.rollbackTransaction"),
  params: SessionRollbackTransactionParamsSchema.default({}),
  risk: z.enum(["low", "medium", "high"])
});

export const SceneModifyActorActionSchema = PlanActionSchema;
export const SceneCreateActorActionSchema = z.object({
  command: z.literal("scene.createActor"),
  params: SceneCreateActorParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneDeleteActorActionSchema = z.object({
  command: z.literal("scene.deleteActor"),
  params: SceneDeleteActorParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneModifyComponentActionSchema = z.object({
  command: z.literal("scene.modifyComponent"),
  params: SceneModifyComponentParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneSetComponentMaterialActionSchema = z.object({
  command: z.literal("scene.setComponentMaterial"),
  params: SceneSetComponentMaterialParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneSetComponentStaticMeshActionSchema = z.object({
  command: z.literal("scene.setComponentStaticMesh"),
  params: SceneSetComponentStaticMeshParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneAddActorTagActionSchema = z.object({
  command: z.literal("scene.addActorTag"),
  params: SceneAddActorTagParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneSetActorFolderActionSchema = z.object({
  command: z.literal("scene.setActorFolder"),
  params: SceneSetActorFolderParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneAddActorLabelPrefixActionSchema = z.object({
  command: z.literal("scene.addActorLabelPrefix"),
  params: SceneAddActorLabelPrefixParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneDuplicateActorsActionSchema = z.object({
  command: z.literal("scene.duplicateActors"),
  params: SceneDuplicateActorsParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneSetDirectionalLightIntensityActionSchema = z.object({
  command: z.literal("scene.setDirectionalLightIntensity"),
  params: SceneSetDirectionalLightIntensityParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneSetFogDensityActionSchema = z.object({
  command: z.literal("scene.setFogDensity"),
  params: SceneSetFogDensityParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const SceneSetPostProcessExposureCompensationActionSchema = z.object({
  command: z.literal("scene.setPostProcessExposureCompensation"),
  params: SceneSetPostProcessExposureCompensationParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const LandscapeSculptActionSchema = z.object({
  command: z.literal("landscape.sculpt"),
  params: LandscapeSculptParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const LandscapePaintLayerActionSchema = z.object({
  command: z.literal("landscape.paintLayer"),
  params: LandscapePaintLayerParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const LandscapeGenerateActionSchema = z.object({
  command: z.literal("landscape.generate"),
  params: LandscapeGenerateParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const PlanActionUnionSchema = z.discriminatedUnion("command", [
  ContextGetSceneSummaryActionSchema,
  ContextGetSelectionActionSchema,
  EditorUndoActionSchema,
  EditorRedoActionSchema,
  SessionBeginTransactionActionSchema,
  SessionCommitTransactionActionSchema,
  SessionRollbackTransactionActionSchema,
  SceneModifyActorActionSchema,
  SceneCreateActorActionSchema,
  SceneDeleteActorActionSchema,
  SceneModifyComponentActionSchema,
  SceneSetComponentMaterialActionSchema,
  SceneSetComponentStaticMeshActionSchema,
  SceneAddActorTagActionSchema,
  SceneSetActorFolderActionSchema,
  SceneAddActorLabelPrefixActionSchema,
  SceneDuplicateActorsActionSchema,
  SceneSetDirectionalLightIntensityActionSchema,
  SceneSetFogDensityActionSchema,
  SceneSetPostProcessExposureCompensationActionSchema,
  LandscapeSculptActionSchema,
  LandscapePaintLayerActionSchema,
  LandscapeGenerateActionSchema
]);

const PlanPrioritySchema = z.enum(["low", "medium", "high"]).default("medium");

export const PlanGoalSchema = z.object({
  id: z.string().min(1),
  description: z.string().min(1),
  priority: PlanPrioritySchema
});

export const PlanSubgoalSchema = z.object({
  id: z.string().min(1),
  description: z.string().min(1),
  dependsOn: z.array(z.string().min(1)).default([])
});

export const PlanCheckSchema = z.object({
  id: z.string().min(1),
  description: z.string().min(1),
  type: z.enum(["constraint", "success", "safety"]),
  source: z.enum(["intent.constraints", "intent.successCriteria", "planner"]).default("planner"),
  status: z.enum(["pending", "passed", "failed", "unknown"]).default("pending"),
  onFail: z.enum(["revise_subgoals", "require_approval", "stop"]).default("revise_subgoals")
});

const StopAllChecksPassedSchema = z.object({
  type: z.literal("all_checks_passed")
});

const StopMaxIterationsSchema = z.object({
  type: z.literal("max_iterations"),
  value: z.number().int().min(1)
});

const StopNoProgressSchema = z.object({
  type: z.literal("no_progress"),
  iterations: z.number().int().min(1)
});

const StopRiskThresholdSchema = z.object({
  type: z.literal("risk_threshold"),
  maxRisk: z.enum(["low", "medium", "high"])
});

const StopUserDeniedSchema = z.object({
  type: z.literal("user_denied")
});

const StopManualStopSchema = z.object({
  type: z.literal("manual_stop")
});

export const PlanStopConditionSchema = z.discriminatedUnion("type", [
  StopAllChecksPassedSchema,
  StopMaxIterationsSchema,
  StopNoProgressSchema,
  StopRiskThresholdSchema,
  StopUserDeniedSchema,
  StopManualStopSchema
]);

export const PlanOutputSchema = z.object({
  summary: z.string().min(1),
  steps: z.array(z.string().min(1)).min(1),
  actions: z.array(PlanActionUnionSchema).default([]),
  goal: PlanGoalSchema.default({
    id: "goal_primary",
    description: "Execute the requested Unreal Editor task.",
    priority: "medium"
  }),
  subgoals: z.array(PlanSubgoalSchema).default([]),
  checks: z.array(PlanCheckSchema).default([]),
  stopConditions: z
    .array(PlanStopConditionSchema)
    .default([{ type: "all_checks_passed" }, { type: "max_iterations", value: 1 }, { type: "user_denied" }])
}).superRefine((value, ctx) => {
  const subgoalIds = new Set<string>();
  for (const subgoal of value.subgoals) {
    if (subgoalIds.has(subgoal.id)) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: `Duplicate subgoal id: ${subgoal.id}`
      });
    }
    subgoalIds.add(subgoal.id);
  }

  for (const subgoal of value.subgoals) {
    for (const dep of subgoal.dependsOn) {
      if (!subgoalIds.has(dep)) {
        ctx.addIssue({
          code: z.ZodIssueCode.custom,
          message: `Subgoal ${subgoal.id} depends on missing subgoal id: ${dep}`
        });
      }
    }
  }

  const checkIds = new Set<string>();
  for (const check of value.checks) {
    if (checkIds.has(check.id)) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: `Duplicate check id: ${check.id}`
      });
    }
    checkIds.add(check.id);
  }
});

export type PlanAction = z.infer<typeof PlanActionUnionSchema>;
export type PlanOutput = z.infer<typeof PlanOutputSchema>;

export const SessionStartRequestSchema = TaskRequestSchema.extend({
  maxRetries: z.number().int().min(0).max(10).default(2)
});
export type SessionStartRequest = z.infer<typeof SessionStartRequestSchema>;

export const SessionResultSchema = z.object({
  actionIndex: z.number().int().min(0),
  ok: z.boolean(),
  message: z.string().optional()
});
export type SessionResult = z.infer<typeof SessionResultSchema>;

export const SessionNextRequestSchema = z.object({
  sessionId: z.string().min(1),
  result: SessionResultSchema.optional(),
  chatId: z.string().uuid().optional()
});
export type SessionNextRequest = z.infer<typeof SessionNextRequestSchema>;

export const SessionApproveRequestSchema = z.object({
  sessionId: z.string().min(1),
  actionIndex: z.number().int().min(0),
  approved: z.boolean(),
  chatId: z.string().uuid().optional()
});
export type SessionApproveRequest = z.infer<typeof SessionApproveRequestSchema>;

export const SessionResumeRequestSchema = z.object({
  sessionId: z.string().min(1),
  chatId: z.string().uuid().optional()
});
export type SessionResumeRequest = z.infer<typeof SessionResumeRequestSchema>;

export const ChatCreateRequestSchema = z.object({
  title: z.string().trim().min(1).max(120).optional()
});
export type ChatCreateRequest = z.infer<typeof ChatCreateRequestSchema>;

export const ChatUpdateRequestSchema = z
  .object({
    title: z.string().trim().min(1).max(120).optional(),
    archived: z.boolean().optional()
  })
  .refine((value) => value.title !== undefined || value.archived !== undefined, {
    message: "At least one field must be provided."
  });
export type ChatUpdateRequest = z.infer<typeof ChatUpdateRequestSchema>;

export const ChatDetailAppendRequestSchema = z.object({
  route: z.string().trim().min(1).max(120),
  summary: z.string().trim().min(1).max(500),
  payload: z.record(z.unknown()).optional()
});
export type ChatDetailAppendRequest = z.infer<typeof ChatDetailAppendRequestSchema>;
