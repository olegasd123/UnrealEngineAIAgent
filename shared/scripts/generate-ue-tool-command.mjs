import { readFile, writeFile, mkdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const schemaPath = path.resolve(__dirname, "../schemas/ue-tool-command.schema.json");
const outputPath = path.resolve(__dirname, "../../agent-core/src/generated/ueToolCommandSchema.ts");
const ueHeaderPath = path.resolve(
  __dirname,
  "../../ue-plugin/UEAIAgent/Source/UEAIAgentTransport/Public/UEAIAgentToolCommands.h"
);

function assertArrayOfStrings(value, label) {
  if (!Array.isArray(value) || value.some((item) => typeof item !== "string")) {
    throw new Error(`Expected ${label} to be an array of strings.`);
  }
}

async function main() {
  const schemaRaw = await readFile(schemaPath, "utf8");
  const schema = JSON.parse(schemaRaw);
  const commands = schema?.properties?.command?.enum;
  assertArrayOfStrings(commands, "properties.command.enum");

  const lines = [
    "/*",
    " * Generated from shared/schemas/ue-tool-command.schema.json.",
    " * Do not edit manually.",
    " */",
    "",
    "import { z } from \"zod\";",
    "",
    "export const AllowedCommands = [",
    ...commands.map((command) => `  ${JSON.stringify(command)},`),
    "] as const;",
    "",
    "export type AllowedCommand = (typeof AllowedCommands)[number];",
    "",
    "export const UeToolCommandSchema = z",
    "  .object({",
    "    command: z.enum(AllowedCommands),",
    "    params: z.record(z.unknown())",
    "  })",
    "  .strict();",
    "",
    "export type UeToolCommand = z.infer<typeof UeToolCommandSchema>;",
    ""
  ].join("\n");

  await mkdir(path.dirname(outputPath), { recursive: true });
  await writeFile(outputPath, lines, "utf8");

  const headerLines = [
    "/*",
    " * Generated from shared/schemas/ue-tool-command.schema.json.",
    " * Do not edit manually.",
    " */",
    "#pragma once",
    "",
    "#include \"CoreMinimal.h\"",
    "",
    "namespace UEAIAgentToolCommands",
    "{",
    "    static constexpr int32 CommandCount = " + commands.length + ";",
    "    static const TCHAR* const Commands[CommandCount] = {",
    ...commands.map((command) => `        TEXT(${JSON.stringify(command)}),`),
    "    };",
    "}",
    ""
  ].join("\n");

  await mkdir(path.dirname(ueHeaderPath), { recursive: true });
  await writeFile(ueHeaderPath, headerLines, "utf8");
}

main().catch((error) => {
  // eslint-disable-next-line no-console
  console.error(error);
  process.exitCode = 1;
});
