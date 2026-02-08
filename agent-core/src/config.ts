import { z } from "zod";

const ProviderSchema = z.enum(["openai", "gemini", "local"]);

const PortSchema = z.preprocess(
  (value) => {
    if (value === undefined || value === null || value === "") {
      return 4317;
    }
    return Number(value);
  },
  z.number().int().min(1).max(65535)
);

const TemperatureSchema = z.preprocess(
  (value) => {
    if (value === undefined || value === null || value === "") {
      return 0.2;
    }
    return Number(value);
  },
  z.number().min(0).max(2)
);

const MaxTokensSchema = z.preprocess(
  (value) => {
    if (value === undefined || value === null || value === "") {
      return 1200;
    }
    return Number(value);
  },
  z.number().int().min(1)
);

const PositiveIntSchema = z.preprocess(
  (value) => {
    if (value === undefined || value === null || value === "") {
      return undefined;
    }
    return Number(value);
  },
  z.number().int().min(1)
);

const RawEnvSchema = z.object({
  AGENT_HOST: z.string().trim().min(1).default("127.0.0.1"),
  AGENT_PORT: PortSchema,
  AGENT_PROVIDER: ProviderSchema.default("local"),
  AGENT_TASK_LOG_PATH: z.string().trim().min(1).default("data"),
  AGENT_DB_PATH: z.string().trim().min(1).default("data/agent.db"),

  OPENAI_API_KEY: z.string().trim().optional(),
  OPENAI_MODEL: z.string().trim().min(1).default("gpt-4.1-mini"),
  OPENAI_BASE_URL: z.string().trim().optional(),
  OPENAI_TEMPERATURE: TemperatureSchema,
  OPENAI_MAX_TOKENS: MaxTokensSchema,

  GEMINI_API_KEY: z.string().trim().optional(),
  GEMINI_MODEL: z.string().trim().min(1).default("gemini-2.0-flash"),
  GEMINI_BASE_URL: z.string().trim().optional(),
  GEMINI_TEMPERATURE: TemperatureSchema,
  GEMINI_MAX_TOKENS: MaxTokensSchema,

  LOCAL_API_KEY: z.string().trim().optional(),
  LOCAL_MODEL: z.string().trim().min(1).default("openai/gpt-oss-20b"),
  LOCAL_BASE_URL: z.string().trim().optional(),
  LOCAL_TEMPERATURE: TemperatureSchema,
  LOCAL_MAX_TOKENS: MaxTokensSchema,

  AGENT_POLICY_MAX_CREATE_COUNT: PositiveIntSchema.default(50),
  AGENT_POLICY_MAX_DUPLICATE_COUNT: PositiveIntSchema.default(10),
  AGENT_POLICY_MAX_TARGET_NAMES: PositiveIntSchema.default(50),
  AGENT_POLICY_MAX_DELETE_BY_NAME_COUNT: PositiveIntSchema.default(20),
  AGENT_POLICY_SELECTION_TARGET_ESTIMATE: PositiveIntSchema.default(5),
  AGENT_POLICY_MAX_SESSION_CHANGE_UNITS: PositiveIntSchema.default(120)
});

function optionalNonEmpty(value: string | undefined): string | undefined {
  if (!value) {
    return undefined;
  }
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

const env = RawEnvSchema.parse(process.env);

export type ProviderName = z.infer<typeof ProviderSchema>;

export interface ProviderRuntimeConfig {
  apiKey?: string;
  model: string;
  baseUrl?: string;
  temperature: number;
  maxTokens: number;
}

export interface PolicyRuntimeConfig {
  maxCreateCount: number;
  maxDuplicateCount: number;
  maxTargetNames: number;
  maxDeleteByNameCount: number;
  selectionTargetEstimate: number;
  maxSessionChangeUnits: number;
}

export const config = {
  host: env.AGENT_HOST,
  port: env.AGENT_PORT,
  provider: env.AGENT_PROVIDER as ProviderName,
  taskLogPath: env.AGENT_TASK_LOG_PATH,
  dbPath: env.AGENT_DB_PATH,
  providers: {
    openai: {
      apiKey: optionalNonEmpty(env.OPENAI_API_KEY),
      model: env.OPENAI_MODEL,
      baseUrl: optionalNonEmpty(env.OPENAI_BASE_URL),
      temperature: env.OPENAI_TEMPERATURE,
      maxTokens: env.OPENAI_MAX_TOKENS
    },
    gemini: {
      apiKey: optionalNonEmpty(env.GEMINI_API_KEY),
      model: env.GEMINI_MODEL,
      baseUrl: optionalNonEmpty(env.GEMINI_BASE_URL),
      temperature: env.GEMINI_TEMPERATURE,
      maxTokens: env.GEMINI_MAX_TOKENS
    },
    local: {
      apiKey: optionalNonEmpty(env.LOCAL_API_KEY),
      model: env.LOCAL_MODEL,
      baseUrl: optionalNonEmpty(env.LOCAL_BASE_URL),
      temperature: env.LOCAL_TEMPERATURE,
      maxTokens: env.LOCAL_MAX_TOKENS
    }
  },
  policy: {
    maxCreateCount: env.AGENT_POLICY_MAX_CREATE_COUNT,
    maxDuplicateCount: env.AGENT_POLICY_MAX_DUPLICATE_COUNT,
    maxTargetNames: env.AGENT_POLICY_MAX_TARGET_NAMES,
    maxDeleteByNameCount: env.AGENT_POLICY_MAX_DELETE_BY_NAME_COUNT,
    selectionTargetEstimate: env.AGENT_POLICY_SELECTION_TARGET_ESTIMATE,
    maxSessionChangeUnits: env.AGENT_POLICY_MAX_SESSION_CHANGE_UNITS
  }
};
