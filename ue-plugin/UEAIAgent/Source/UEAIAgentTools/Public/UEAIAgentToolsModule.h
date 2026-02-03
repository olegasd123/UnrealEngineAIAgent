#pragma once

#include "Modules/ModuleInterface.h"

class FUEAIAgentToolsModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};

