#pragma once

#include "Modules/ModuleInterface.h"

class FUEAIAgentTransportModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};

