#include "UEAIAgentTransportModule.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
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
    return TEXT("http://127.0.0.1:4317/health");
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

IMPLEMENT_MODULE(FUEAIAgentTransportModule, UEAIAgentTransport)
