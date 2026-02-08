#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UEAIAgentSettings.generated.h"

UENUM()
enum class EUEAIAgentProvider : uint8
{
    OpenAI UMETA(DisplayName = "OpenAI"),
    Gemini UMETA(DisplayName = "Gemini"),
    Local UMETA(DisplayName = "Local")
};

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "UE AI Agent"))
class UEAIAGENTTRANSPORT_API UUEAIAgentSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UUEAIAgentSettings();

    virtual FName GetContainerName() const override;
    virtual FName GetCategoryName() const override;

    UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Agent Host"))
    FString AgentHost;

    UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (DisplayName = "Agent Port", ClampMin = "1", ClampMax = "65535"))
    int32 AgentPort;

    UPROPERTY(Config, EditAnywhere, Category = "Provider", meta = (DisplayName = "Default Provider"))
    EUEAIAgentProvider DefaultProvider;
};
