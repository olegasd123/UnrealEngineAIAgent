import { z } from "zod";

export const TaskRequestSchema = z.object({
  prompt: z.string().min(1),
  mode: z.enum(["chat", "agent"]).default("chat"),
  context: z.record(z.unknown()).default({})
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

const SceneModifyActorParamsSchema = z
  .object({
    target: z.enum(["selection", "byName"]),
    actorNames: z.array(z.string().min(1)).optional(),
    deltaLocation: DeltaLocationSchema.optional(),
    deltaRotation: DeltaRotationSchema.optional(),
    deltaScale: DeltaScaleSchema.optional()
  })
  .refine((value) => Boolean(value.deltaLocation || value.deltaRotation || value.deltaScale), {
    message: "scene.modifyActor action needs deltaLocation, deltaRotation, or deltaScale"
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
    visibility: z.boolean().optional()
  })
  .refine((value) => Boolean(value.deltaLocation || value.deltaRotation || value.deltaScale || value.visibility !== undefined), {
    message: "scene.modifyComponent action needs a delta transform or visibility"
  })
  .refine((value) => (value.target === "byName" ? (value.actorNames ?? []).length > 0 : true), {
    message: "scene.modifyComponent target=byName needs actorNames"
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

export const SceneAddActorTagActionSchema = z.object({
  command: z.literal("scene.addActorTag"),
  params: SceneAddActorTagParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const PlanActionUnionSchema = z.discriminatedUnion("command", [
  SceneModifyActorActionSchema,
  SceneCreateActorActionSchema,
  SceneDeleteActorActionSchema,
  SceneModifyComponentActionSchema,
  SceneAddActorTagActionSchema
]);

export const PlanOutputSchema = z.object({
  summary: z.string().min(1),
  steps: z.array(z.string().min(1)).min(1),
  actions: z.array(PlanActionUnionSchema).default([])
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
  result: SessionResultSchema.optional()
});
export type SessionNextRequest = z.infer<typeof SessionNextRequestSchema>;

export const SessionApproveRequestSchema = z.object({
  sessionId: z.string().min(1),
  actionIndex: z.number().int().min(0),
  approved: z.boolean()
});
export type SessionApproveRequest = z.infer<typeof SessionApproveRequestSchema>;

export const SessionResumeRequestSchema = z.object({
  sessionId: z.string().min(1)
});
export type SessionResumeRequest = z.infer<typeof SessionResumeRequestSchema>;
