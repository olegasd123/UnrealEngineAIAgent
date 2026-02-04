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

const SceneModifyActorParamsSchema = z
  .object({
    target: z.literal("selection"),
    deltaLocation: DeltaLocationSchema.optional(),
    deltaRotation: DeltaRotationSchema.optional()
  })
  .refine((value) => Boolean(value.deltaLocation || value.deltaRotation), {
    message: "scene.modifyActor action needs deltaLocation or deltaRotation"
  });

export const PlanActionSchema = z.object({
  command: z.literal("scene.modifyActor"),
  params: SceneModifyActorParamsSchema,
  risk: z.enum(["low", "medium", "high"])
});

export const PlanOutputSchema = z.object({
  summary: z.string().min(1),
  steps: z.array(z.string().min(1)).min(1),
  actions: z.array(PlanActionSchema).default([])
});

export type PlanAction = z.infer<typeof PlanActionSchema>;
export type PlanOutput = z.infer<typeof PlanOutputSchema>;
