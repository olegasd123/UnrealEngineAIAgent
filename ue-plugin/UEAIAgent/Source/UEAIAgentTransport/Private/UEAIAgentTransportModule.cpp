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

FString FUEAIAgentTransportModule::BuildBaseUrl() const
{
    const UUEAIAgentSettings* Settings = GetDefault<UUEAIAgentSettings>();
    const FString Host = Settings ? Settings->AgentHost : TEXT("127.0.0.1");
    const int32 Port = Settings ? Settings->AgentPort : 4317;
    return FString::Printf(TEXT("http://%s:%d"), *Host, Port);
}

FString FUEAIAgentTransportModule::BuildHealthUrl() const
{
    return BuildBaseUrl() + TEXT("/health");
}

FString FUEAIAgentTransportModule::BuildPlanUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/task/plan");
}

FString FUEAIAgentTransportModule::BuildProviderStatusUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/providers/status");
}

FString FUEAIAgentTransportModule::BuildCredentialsSetUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/credentials/set");
}

FString FUEAIAgentTransportModule::BuildCredentialsDeleteUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/credentials/delete");
}

FString FUEAIAgentTransportModule::BuildCredentialsTestUrl() const
{
    return BuildBaseUrl() + TEXT("/v1/credentials/test");
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
    PlannedActions.Empty();

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

                        const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
                        if (!ActionObj->TryGetObjectField(TEXT("params"), ParamsObj) || !ParamsObj || !ParamsObj->IsValid())
                        {
                            continue;
                        }

                        FString Command;
                        if (!ActionObj->TryGetStringField(TEXT("command"), Command))
                        {
                            continue;
                        }

                        if (Command == TEXT("scene.modifyActor"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target) || Target != TEXT("selection"))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::ModifyActor;
                            ParsedAction.ActorNames = SelectedActors;

                            bool bHasAnyDelta = false;
                            const TSharedPtr<FJsonObject>* DeltaLocationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaLocation"), DeltaLocationObj) &&
                                DeltaLocationObj && DeltaLocationObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*DeltaLocationObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*DeltaLocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*DeltaLocationObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.DeltaLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                    bHasAnyDelta = true;
                                }
                            }

                            const TSharedPtr<FJsonObject>* DeltaRotationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("deltaRotation"), DeltaRotationObj) &&
                                DeltaRotationObj && DeltaRotationObj->IsValid())
                            {
                                double Pitch = 0.0;
                                double Yaw = 0.0;
                                double Roll = 0.0;
                                if ((*DeltaRotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                                    (*DeltaRotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                                    (*DeltaRotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                                {
                                    ParsedAction.DeltaRotation = FRotator(
                                        static_cast<float>(Pitch),
                                        static_cast<float>(Yaw),
                                        static_cast<float>(Roll));
                                    bHasAnyDelta = true;
                                }
                            }

                            if (bHasAnyDelta)
                            {
                                PlannedActions.Add(ParsedAction);
                            }
                            continue;
                        }

                        if (Command == TEXT("scene.createActor"))
                        {
                            FString ActorClass;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("actorClass"), ActorClass) || ActorClass.IsEmpty())
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::CreateActor;
                            ParsedAction.ActorClass = ActorClass;

                            double Count = 1.0;
                            if ((*ParamsObj)->TryGetNumberField(TEXT("count"), Count))
                            {
                                ParsedAction.SpawnCount = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Count)), 1, 200);
                            }

                            const TSharedPtr<FJsonObject>* LocationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("location"), LocationObj) &&
                                LocationObj && LocationObj->IsValid())
                            {
                                double X = 0.0;
                                double Y = 0.0;
                                double Z = 0.0;
                                if ((*LocationObj)->TryGetNumberField(TEXT("x"), X) &&
                                    (*LocationObj)->TryGetNumberField(TEXT("y"), Y) &&
                                    (*LocationObj)->TryGetNumberField(TEXT("z"), Z))
                                {
                                    ParsedAction.SpawnLocation = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
                                }
                            }

                            const TSharedPtr<FJsonObject>* RotationObj = nullptr;
                            if ((*ParamsObj)->TryGetObjectField(TEXT("rotation"), RotationObj) &&
                                RotationObj && RotationObj->IsValid())
                            {
                                double Pitch = 0.0;
                                double Yaw = 0.0;
                                double Roll = 0.0;
                                if ((*RotationObj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
                                    (*RotationObj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
                                    (*RotationObj)->TryGetNumberField(TEXT("roll"), Roll))
                                {
                                    ParsedAction.SpawnRotation = FRotator(
                                        static_cast<float>(Pitch),
                                        static_cast<float>(Yaw),
                                        static_cast<float>(Roll));
                                }
                            }

                            PlannedActions.Add(ParsedAction);
                            continue;
                        }

                        if (Command == TEXT("scene.deleteActor"))
                        {
                            FString Target;
                            if (!(*ParamsObj)->TryGetStringField(TEXT("target"), Target) || Target != TEXT("selection"))
                            {
                                continue;
                            }

                            FUEAIAgentPlannedSceneAction ParsedAction;
                            ParsedAction.Type = EUEAIAgentPlannedActionType::DeleteActor;
                            ParsedAction.ActorNames = SelectedActors;
                            PlannedActions.Add(ParsedAction);
                        }
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

void FUEAIAgentTransportModule::SetProviderApiKey(
    const FString& Provider,
    const FString& ApiKey,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("provider"), Provider);
    Root->SetStringField(TEXT("apiKey"), ApiKey);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildCredentialsSetUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Save key failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                Callback.ExecuteIfBound(true, TEXT("API key saved."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::DeleteProviderApiKey(
    const FString& Provider,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("provider"), Provider);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildCredentialsDeleteUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Delete key failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                Callback.ExecuteIfBound(true, TEXT("API key removed."));
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::TestProviderApiKey(
    const FString& Provider,
    const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("provider"), Provider);

    FString RequestBody;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);
    FJsonSerializer::Serialize(Root, Writer);

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildCredentialsTestUrl());
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetContentAsString(RequestBody);
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Test key failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Test response is not valid JSON."));
                    return;
                }

                bool bOk = false;
                ResponseJson->TryGetBoolField(TEXT("ok"), bOk);
                FString Message = bOk ? TEXT("Provider call succeeded.") : TEXT("Provider call failed.");
                ResponseJson->TryGetStringField(TEXT("message"), Message);
                Callback.ExecuteIfBound(bOk, Message);
            });
        });

    Request->ProcessRequest();
}

void FUEAIAgentTransportModule::GetProviderStatus(const FOnUEAIAgentCredentialOpFinished& Callback) const
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(BuildProviderStatusUrl());
    Request->SetVerb(TEXT("GET"));
    Request->OnProcessRequestComplete().BindLambda(
        [Callback](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
        {
            AsyncTask(ENamedThreads::GameThread, [Callback, HttpResponse, bConnectedSuccessfully]()
            {
                if (!bConnectedSuccessfully || !HttpResponse.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Could not connect to Agent Core."));
                    return;
                }

                if (HttpResponse->GetResponseCode() < 200 || HttpResponse->GetResponseCode() >= 300)
                {
                    Callback.ExecuteIfBound(false, FString::Printf(TEXT("Provider status failed (%d)."), HttpResponse->GetResponseCode()));
                    return;
                }

                TSharedPtr<FJsonObject> ResponseJson;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(HttpResponse->GetContentAsString());
                if (!FJsonSerializer::Deserialize(Reader, ResponseJson) || !ResponseJson.IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Provider status response is not valid JSON."));
                    return;
                }

                const TSharedPtr<FJsonObject>* ProvidersObj = nullptr;
                if (!ResponseJson->TryGetObjectField(TEXT("providers"), ProvidersObj) || !ProvidersObj || !ProvidersObj->IsValid())
                {
                    Callback.ExecuteIfBound(false, TEXT("Provider status misses providers object."));
                    return;
                }

                auto BuildLine = [ProvidersObj](const FString& ProviderName) -> FString
                {
                    const TSharedPtr<FJsonObject>* ProviderObj = nullptr;
                    if (!(*ProvidersObj)->TryGetObjectField(ProviderName, ProviderObj) || !ProviderObj || !ProviderObj->IsValid())
                    {
                        return FString::Printf(TEXT("%s: unknown"), *ProviderName);
                    }

                    bool bConfigured = false;
                    (*ProviderObj)->TryGetBoolField(TEXT("configured"), bConfigured);
                    FString Model;
                    (*ProviderObj)->TryGetStringField(TEXT("model"), Model);

                    if (Model.IsEmpty())
                    {
                        return FString::Printf(TEXT("%s: %s"), *ProviderName, bConfigured ? TEXT("configured") : TEXT("not configured"));
                    }

                    return FString::Printf(
                        TEXT("%s: %s (%s)"),
                        *ProviderName,
                        bConfigured ? TEXT("configured") : TEXT("not configured"),
                        *Model);
                };

                const FString Message = BuildLine(TEXT("openai")) + TEXT("\n") + BuildLine(TEXT("gemini"));
                Callback.ExecuteIfBound(true, Message);
            });
        });

    Request->ProcessRequest();
}

int32 FUEAIAgentTransportModule::GetPlannedActionCount() const
{
    return PlannedActions.Num();
}

FString FUEAIAgentTransportModule::GetPlannedActionPreviewText(int32 ActionIndex) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return TEXT("Invalid action index.");
    }

    const FUEAIAgentPlannedSceneAction& Action = PlannedActions[ActionIndex];
    if (Action.Type == EUEAIAgentPlannedActionType::CreateActor)
    {
        return FString::Printf(
            TEXT("Action %d -> scene.createActor count=%d class=%s, Location: X=%.2f Y=%.2f Z=%.2f, Rotation: Pitch=%.2f Yaw=%.2f Roll=%.2f"),
            ActionIndex + 1,
            Action.SpawnCount,
            *Action.ActorClass,
            Action.SpawnLocation.X,
            Action.SpawnLocation.Y,
            Action.SpawnLocation.Z,
            Action.SpawnRotation.Pitch,
            Action.SpawnRotation.Yaw,
            Action.SpawnRotation.Roll);
    }

    if (Action.Type == EUEAIAgentPlannedActionType::DeleteActor)
    {
        return FString::Printf(
            TEXT("Action %d -> scene.deleteActor on selection (%d actor(s))"),
            ActionIndex + 1,
            Action.ActorNames.Num());
    }

    return FString::Printf(
        TEXT("Action %d -> scene.modifyActor on %d actor(s), DeltaLocation: X=%.2f Y=%.2f Z=%.2f, DeltaRotation: Pitch=%.2f Yaw=%.2f Roll=%.2f"),
        ActionIndex + 1,
        Action.ActorNames.Num(),
        Action.DeltaLocation.X,
        Action.DeltaLocation.Y,
        Action.DeltaLocation.Z,
        Action.DeltaRotation.Pitch,
        Action.DeltaRotation.Yaw,
        Action.DeltaRotation.Roll);
}

bool FUEAIAgentTransportModule::IsPlannedActionApproved(int32 ActionIndex) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return false;
    }

    return PlannedActions[ActionIndex].bApproved;
}

void FUEAIAgentTransportModule::SetPlannedActionApproved(int32 ActionIndex, bool bApproved) const
{
    if (!PlannedActions.IsValidIndex(ActionIndex))
    {
        return;
    }

    PlannedActions[ActionIndex].bApproved = bApproved;
}

bool FUEAIAgentTransportModule::PopApprovedPlannedActions(TArray<FUEAIAgentPlannedSceneAction>& OutActions) const
{
    OutActions.Empty();
    for (const FUEAIAgentPlannedSceneAction& Action : PlannedActions)
    {
        if (Action.bApproved)
        {
            OutActions.Add(Action);
        }
    }

    PlannedActions.Empty();
    return OutActions.Num() > 0;
}

IMPLEMENT_MODULE(FUEAIAgentTransportModule, UEAIAgentTransport)
