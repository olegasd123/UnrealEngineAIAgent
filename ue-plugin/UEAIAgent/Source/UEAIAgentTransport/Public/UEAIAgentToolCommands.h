/*
 * Generated from shared/schemas/ue-tool-command.schema.json.
 * Do not edit manually.
 */
#pragma once

#include "CoreMinimal.h"

namespace UEAIAgentToolCommands
{
    static constexpr int32 CommandCount = 23;
    static const TCHAR* const Commands[CommandCount] = {
        TEXT("context.getSceneSummary"),
        TEXT("context.getSelection"),
        TEXT("scene.setDirectionalLightIntensity"),
        TEXT("scene.setFogDensity"),
        TEXT("scene.setPostProcessExposureCompensation"),
        TEXT("scene.createActor"),
        TEXT("scene.modifyActor"),
        TEXT("scene.deleteActor"),
        TEXT("scene.modifyComponent"),
        TEXT("scene.setComponentMaterial"),
        TEXT("scene.setComponentStaticMesh"),
        TEXT("scene.addActorTag"),
        TEXT("scene.setActorFolder"),
        TEXT("scene.addActorLabelPrefix"),
        TEXT("scene.duplicateActors"),
        TEXT("landscape.sculpt"),
        TEXT("landscape.paintLayer"),
        TEXT("landscape.generate"),
        TEXT("editor.undo"),
        TEXT("editor.redo"),
        TEXT("session.beginTransaction"),
        TEXT("session.commitTransaction"),
        TEXT("session.rollbackTransaction"),
    };
}
