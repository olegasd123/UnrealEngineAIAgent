#pragma once

#include "Modules/ModuleInterface.h"

class FUEAIAgentEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};

