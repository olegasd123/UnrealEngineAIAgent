#include "UEAIAgentTransportModule.h"

#include "UEAIAgentSettings.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIAgentTransport, Log, All);

void FUEAIAgentTransportModule::StartupModule()
{
    UE_LOG(LogUEAIAgentTransport, Log, TEXT("UEAIAgentTransport started."));
}

void FUEAIAgentTransportModule::ShutdownModule()
{
    UE_LOG(LogUEAIAgentTransport, Log, TEXT("UEAIAgentTransport stopped."));
}

FString FUEAIAgentTransportModule::BuildHealthUrl() const
{
    const UUEAIAgentSettings* Settings = GetDefault<UUEAIAgentSettings>();
    const FString Host = Settings ? Settings->AgentHost : TEXT("127.0.0.1");
    const int32 Port = Settings ? Settings->AgentPort : 4317;
    return FString::Printf(TEXT("http://%s:%d/health"), *Host, Port);
}

FString FUEAIAgentTransportModule::BuildPlanUrl() const
{
    const UUEAIAgentSettings* Settings = GetDefault<UUEAIAgentSettings>();
    const FString Host = Settings ? Settings->AgentHost : TEXT("127.0.0.1");
    const int32 Port = Settings ? Settings->AgentPort : 4317;
    return FString::Printf(TEXT("http://%s:%d/v1/task/plan"), *Host, Port);
}

void FUEAIAgentTransportModule::CheckHealth(const FOnUEAIAgentHealthChecked& Callback) const
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildHealthUrl());
    Request->SetVerb(TEXT("GET"));

    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Agent Core is not reachable."));
                    return;
                }

                const int32 StatusCode = HttpResponse->GetResponseCode();
                if (StatusCode < 200 || StatusCode >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Health check failed (%d)."), StatusCode));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Health response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk))
                {
                    Callback.ExecuteIfBound(false, TEXT("Health response misses 'ok' field."));
                    return;
                }

                FString Provider;
                ResponseJson->TryGetStringField(TEXT("provider"), Provider);
                if (!bOk)
                {
                    Callback.ExecuteIfBound(false, TEXT("Agent Core reports unhealthy state."));
                    return;
                }

                const FString Message = Provider.IsEmpty()
                    ? TEXT("Connected.")
                    : FString::Printf(TEXT("Connected. Provider: %s"), *Provider);
                Callback.ExecuteIfBound(true, Message);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::PlanTask(const FString& Prompt, const TArray<FString>& SelectedActors, const FOnUEAIAgentTaskPlanned& Callback) const
{
    bHasPlannedMoveAction = false;
    PlannedActorNames.Empty();
    PlannedDeltaLocation = FVector::ZeroVector;

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("prompt"), Prompt);
    Root->SetStringField(TEXT("mode"), TEXT("chat"));

    TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> SelectionValues;
    for (const FString& ActorName : SelectedActors)
    {
        SelectionValues.Add(MakeShared<FJsonValueString>(ActorName));
    }
    Context->SetArrayField(TEXT("selection"), SelectionValues);
    Root->SetObjectField(TEXT("context"), Context);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildPlanUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);

    Request->OnProcessRequestComplete().BindLambda(
        [this, Callback, SelectedActors](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [this, Callback, SelectedActors, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(
                        false,
                        FString::Printf(TEXT("Plan request failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Plan response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                if (!ResponseJson->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
                {
                    FString ErrorMessage = TEXT("Agent Core returned an error.");
                    ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
                    Callback.ExecuteIfBound(false, ErrorMessage);
                    return;
                }

                const TSharedPtr<FJsonObject>* PlanObj = nullptr;
                if (!ResponseJson->TryGetObjectField(TEXT("plan"), PlanObj) || !PlanObj || !PlanObj->IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Plan response misses 'plan' object."));
                    return;
                }

                FString Summary;
                (*PlanObj)->TryGetStringField(TEXT("summary"), Summary);

                FString StepsText;
                const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
                if ((*PlanObj)->TryGetArrayField(TEXT("steps"), Steps) && Steps)
                {
                    for (int32 StepIndex = 0; StepIndex < Steps->Num(); ++StepIndex)
                    {
                        FString StepValue;
                        if ((*Steps)[StepIndex].IsValid() && (*Steps)[StepIndex]->TryGetString(StepValue))
                        {
                            StepsText += FString::Printf(TEXT("%d. %s\n"), StepIndex + 1, *StepValue);
                        }
                    }
                }

                const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
                if ((*PlanObj)->TryGetArrayField(TEXT("actions"), Actions) && Actions)
                {
                    for (const TSharedPtr<FJsonValue>& ActionValue : *Actions)
                    {
                        if (!ActionValue.IsValid())
                        {
                            continue;
                        }

                        const TSharedPtr<FJsonObject> ActionObj = ActionValue->AsObject();
                        if (!ActionObj.IsValid())
                        {
                            continue;
                        }

                        FString Command;
                        if (!ActionObj->TryGetStringField(TEXT("command"), Command) || Command != TEXT("scene.modifyActor"))
                        {
                            continue;
                        }

                        const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
                        const TSharedPtr<FJsonObject>* DeltaObj = nullptr;
                        if (!ActionObj->TryGetObjectField(TEXT("params"), ParamsObj) || !ParamsObj || !ParamsObj->IsValid())
                        {
                            continue;
                        }

                        if (!(*ParamsObj)->TryGetObjectField(TEXT("deltaLocation"), DeltaObj) || !DeltaObj || !DeltaObj->IsValid())
                        {
                            continue;
                        }

                        double X = 0.0;
                        double Y = 0.0;
                        double Z = 0.0;
                        if (!(*DeltaObj)->TryGetNumberField(TEXT("x"), X) ||
                            !(*DeltaObj)->TryGetNumberField(TEXT("y"), Y) ||
                            !(*DeltaObj)->TryGetNumberField(TEXT("z"), Z))
                        {
                            continue;
                        }

                        bHasPlannedMoveAction = true;
                        PlannedActorNames = SelectedActors;
                        PlannedDeltaLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                        break;
                    }
                }

                const FString FinalMessage = StepsText.IsEmpty()
                    ? Summary
                    : FString::Printf(TEXT("%s\n%s"), *Summary, *StepsText);
                Callback.ExecuteIfBound(true, FinalMessage);
            });
        });

    Request->ProcessRequest();
}

bool FUEAIAgentTransportModule::HasPlannedMoveAction() const
{
    return bHasPlannedMoveAction;
}

bool FUEAIAgentTransportModule::PopPlannedMoveAction(TArray<FString>& OutActorNames, FVector& OutDeltaLocation) const
{
    if (!bHasPlannedMoveAction)
    {
        return false;
    }

    OutActorNames = PlannedActorNames;
    OutDeltaLocation = PlannedDeltaLocation;

    bHasPlannedMoveAction = false;
    PlannedActorNames.Empty();
    PlannedDeltaLocation = FVector::ZeroVector;

    return true;
}

FString FUEAIAgentTransportModule::GetPlannedMovePreviewText() const
{
    if (!bHasPlannedMoveAction)
    {
        return TEXT("No executable scene.modifyActor action in current plan.");
    }

    return FString::Printf(
        TEXT("Preview -> scene.modifyActor on %d actor(s), Delta: X=%.2f Y=%.2f Z=%.2f"),
        PlannedActorNames.Num(),
        PlannedDeltaLocation.X,
        PlannedDeltaLocation.Y,
        PlannedDeltaLocation.Z);
}

IMPLEMENT_MODULE(FUEAIAgentTransportModule, UEAIAgentTransport)
