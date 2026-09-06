// Copyright Roundtree. All Rights Reserved.

#include "LlmShotPlanProvider.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShotGrammarParser.h"
#include "ShotPlanJson.h"

#define LOCTEXT_NAMESPACE "CineDirector"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorLlm, Log, All);

namespace CineLlm
{
	/** The plan schema as a JSON object, parsed once. */
	const TSharedPtr<FJsonObject>& SchemaObject()
	{
		static TSharedPtr<FJsonObject> Schema = []
		{
			TSharedPtr<FJsonObject> Parsed;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FCineShotPlanJson::BuildSchema());
			FJsonSerializer::Deserialize(Reader, Parsed);
			return Parsed;
		}();
		return Schema;
	}

	FString Serialize(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	const TCHAR* EffortToken(ECineLlmEffort Effort)
	{
		switch (Effort)
		{
		case ECineLlmEffort::Low:    return TEXT("low");
		case ECineLlmEffort::Medium: return TEXT("medium");
		default:                     return TEXT("high");
		}
	}

	/** True for the families that speak OpenAI's /chat/completions shape. */
	bool IsOpenAIStyle(ECineShotPlanBackend Backend)
	{
		return Backend == ECineShotPlanBackend::OpenAI
			|| Backend == ECineShotPlanBackend::OpenRouter
			|| Backend == ECineShotPlanBackend::LocalOpenAICompatible
			|| Backend == ECineShotPlanBackend::CustomOpenAICompatible;
	}

	/**
	 * Backends we cannot hand a strict schema to — either the endpoint is an
	 * unknown OpenAI-compatible server whose json_schema support is a coin flip,
	 * or (Gemini) the schema dialect differs. Those get the schema in the prompt
	 * instead, and only a "must be JSON" response constraint.
	 */
	bool NeedsSchemaInPrompt(ECineShotPlanBackend Backend)
	{
		return Backend == ECineShotPlanBackend::LocalOpenAICompatible
			|| Backend == ECineShotPlanBackend::CustomOpenAICompatible
			|| Backend == ECineShotPlanBackend::GoogleGemini;
	}

	/** Backends that need no credential of their own. */
	bool IsKeyOptional(ECineShotPlanBackend Backend)
	{
		return Backend == ECineShotPlanBackend::LocalOpenAICompatible
			|| Backend == ECineShotPlanBackend::CustomOpenAICompatible;
	}

	FText BackendDisplayName(ECineShotPlanBackend Backend)
	{
		switch (Backend)
		{
		case ECineShotPlanBackend::Anthropic:              return LOCTEXT("BackendAnthropic", "Claude");
		case ECineShotPlanBackend::OpenAI:                 return LOCTEXT("BackendOpenAI", "OpenAI");
		case ECineShotPlanBackend::OpenRouter:             return LOCTEXT("BackendOpenRouter", "OpenRouter");
		case ECineShotPlanBackend::LocalOpenAICompatible:  return LOCTEXT("BackendLocal", "Local model");
		case ECineShotPlanBackend::GoogleGemini:           return LOCTEXT("BackendGemini", "Gemini");
		default:                                           return LOCTEXT("BackendCustom", "Custom model");
		}
	}

	/** First line of an error body, so a 400 shows the API's own explanation. */
	FString Summarize(const FString& Body)
	{
		FString Trimmed = Body.TrimStartAndEnd().Replace(TEXT("\n"), TEXT(" "));
		const int32 MaxLength = 400;
		if (Trimmed.Len() > MaxLength)
		{
			Trimmed = Trimmed.Left(MaxLength) + TEXT("…");
		}
		return Trimmed;
	}
}

FLlmShotPlanProvider::FLlmShotPlanProvider(TSharedPtr<IShotPlanProvider> InFallback)
	: Fallback(MoveTemp(InFallback))
{
}

FText FLlmShotPlanProvider::GetProviderName() const
{
	const UCineDirectorSettings* Settings = GetDefault<UCineDirectorSettings>();
	const FString Model = Settings->ResolveModel();

	return Model.IsEmpty()
		? CineLlm::BackendDisplayName(Settings->Backend)
		: FText::Format(LOCTEXT("LlmProviderName", "{0} · {1}"),
			CineLlm::BackendDisplayName(Settings->Backend), FText::FromString(Model));
}

bool FLlmShotPlanProvider::BuildRequest(const UCineDirectorSettings& Settings, const FString& ApiKey,
	const FString& Description, const FCineSceneContext& Scene,
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, FText& OutError) const
{
	using namespace CineLlm;

	const FString Model = Settings.ResolveModel();
	if (Model.IsEmpty())
	{
		OutError = LOCTEXT("LlmNoModel",
			"No model id set. Project Settings → Plugins → CineDirector → Model — this backend has no default "
			"(for example \"anthropic/claude-sonnet-4.5\" on OpenRouter, or \"llama3.1\" on Ollama).");
		return false;
	}

	const FString Endpoint = Settings.ResolveEndpoint();
	if (Endpoint.IsEmpty())
	{
		OutError = LOCTEXT("LlmNoEndpoint",
			"No endpoint URL set. Project Settings → Plugins → CineDirector → Endpoint URL override.");
		return false;
	}

	const TSharedPtr<FJsonObject>& Schema = SchemaObject();
	if (!Schema.IsValid())
	{
		OutError = LOCTEXT("LlmBadSchema", "CineDirector's shot-plan schema failed to parse (this is a bug).");
		return false;
	}

	FString SystemPrompt = FCineShotPlanJson::BuildSystemPrompt();
	FString UserPrompt = FCineShotPlanJson::BuildUserPrompt(
		Description, Scene, Settings.bSendSceneActors, Settings.MaxSceneActors);

	if (NeedsSchemaInPrompt(Settings.Backend))
	{
		// No strict-schema channel on this backend: state the contract in the prompt.
		UserPrompt += TEXT("\nReply with a single JSON object, no markdown fence, matching this JSON Schema exactly:\n")
			+ FCineShotPlanJson::BuildSchema() + TEXT("\n");
	}

	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(Settings.TimeoutSeconds);

	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();

	if (Settings.Backend == ECineShotPlanBackend::Anthropic)
	{
		Request->SetURL(Endpoint);
		Body->SetStringField(TEXT("model"), Model);
		Request->SetHeader(TEXT("x-api-key"), ApiKey);
		Request->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));

		Body->SetNumberField(TEXT("max_tokens"), Settings.MaxOutputTokens);
		Body->SetStringField(TEXT("system"), SystemPrompt);

		const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), TEXT("user"));
		Message->SetStringField(TEXT("content"), UserPrompt);
		Body->SetArrayField(TEXT("messages"), { MakeShared<FJsonValueObject>(Message) });

		// output_config carries both the reasoning-depth hint and the structured
		// output constraint; the response is then a single JSON text block.
		const TSharedRef<FJsonObject> Format = MakeShared<FJsonObject>();
		Format->SetStringField(TEXT("type"), TEXT("json_schema"));
		Format->SetObjectField(TEXT("schema"), Schema);

		const TSharedRef<FJsonObject> OutputConfig = MakeShared<FJsonObject>();
		OutputConfig->SetStringField(TEXT("effort"), EffortToken(Settings.Effort));
		OutputConfig->SetObjectField(TEXT("format"), Format);
		Body->SetObjectField(TEXT("output_config"), OutputConfig);
	}
	else if (IsOpenAIStyle(Settings.Backend))
	{
		Request->SetURL(Endpoint);
		Body->SetStringField(TEXT("model"), Model);
		if (!ApiKey.IsEmpty())
		{
			Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey);
		}
		if (Settings.Backend == ECineShotPlanBackend::OpenRouter)
		{
			// OpenRouter attributes traffic with these; both are optional.
			Request->SetHeader(TEXT("HTTP-Referer"), TEXT("https://github.com/Roundtree/CineDirector"));
			Request->SetHeader(TEXT("X-Title"), TEXT("CineDirector"));
		}

		const TSharedRef<FJsonObject> SystemMessage = MakeShared<FJsonObject>();
		SystemMessage->SetStringField(TEXT("role"), TEXT("system"));
		SystemMessage->SetStringField(TEXT("content"), SystemPrompt);

		const TSharedRef<FJsonObject> UserMessage = MakeShared<FJsonObject>();
		UserMessage->SetStringField(TEXT("role"), TEXT("user"));
		UserMessage->SetStringField(TEXT("content"), UserPrompt);

		Body->SetArrayField(TEXT("messages"), {
			MakeShared<FJsonValueObject>(SystemMessage),
			MakeShared<FJsonValueObject>(UserMessage) });

		// OpenAI retired max_tokens on its newer chat models; everyone else still
		// speaks it and mostly does not know max_completion_tokens.
		Body->SetNumberField(
			Settings.Backend == ECineShotPlanBackend::OpenAI ? TEXT("max_completion_tokens") : TEXT("max_tokens"),
			Settings.MaxOutputTokens);

		const TSharedRef<FJsonObject> ResponseFormat = MakeShared<FJsonObject>();
		if (NeedsSchemaInPrompt(Settings.Backend))
		{
			ResponseFormat->SetStringField(TEXT("type"), TEXT("json_object"));
		}
		else
		{
			const TSharedRef<FJsonObject> JsonSchema = MakeShared<FJsonObject>();
			JsonSchema->SetStringField(TEXT("name"), TEXT("cine_shot_plan"));
			JsonSchema->SetBoolField(TEXT("strict"), true);
			JsonSchema->SetObjectField(TEXT("schema"), Schema);

			ResponseFormat->SetStringField(TEXT("type"), TEXT("json_schema"));
			ResponseFormat->SetObjectField(TEXT("json_schema"), JsonSchema);
		}
		Body->SetObjectField(TEXT("response_format"), ResponseFormat);
	}
	else // Gemini
	{
		// The model is part of the path, and the key rides in a header rather than
		// a query string so it stays out of URLs and logs.
		Request->SetURL(FString::Printf(TEXT("%s/%s:generateContent"), *Endpoint, *Model));
		Request->SetHeader(TEXT("x-goog-api-key"), ApiKey);

		const TSharedRef<FJsonObject> SystemPart = MakeShared<FJsonObject>();
		SystemPart->SetStringField(TEXT("text"), SystemPrompt);
		const TSharedRef<FJsonObject> SystemInstruction = MakeShared<FJsonObject>();
		SystemInstruction->SetArrayField(TEXT("parts"), { MakeShared<FJsonValueObject>(SystemPart) });
		Body->SetObjectField(TEXT("systemInstruction"), SystemInstruction);

		const TSharedRef<FJsonObject> UserPart = MakeShared<FJsonObject>();
		UserPart->SetStringField(TEXT("text"), UserPrompt);
		const TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("role"), TEXT("user"));
		Content->SetArrayField(TEXT("parts"), { MakeShared<FJsonValueObject>(UserPart) });
		Body->SetArrayField(TEXT("contents"), { MakeShared<FJsonValueObject>(Content) });

		const TSharedRef<FJsonObject> GenerationConfig = MakeShared<FJsonObject>();
		GenerationConfig->SetStringField(TEXT("responseMimeType"), TEXT("application/json"));
		GenerationConfig->SetNumberField(TEXT("maxOutputTokens"), Settings.MaxOutputTokens);
		Body->SetObjectField(TEXT("generationConfig"), GenerationConfig);
	}

	const FString Payload = Serialize(Body);
	Request->SetContentAsString(Payload);

	if (Settings.bLogTraffic)
	{
		UE_LOG(LogCineDirectorLlm, Log, TEXT("POST %s\n%s"), *Request->GetURL(), *Payload);
	}

	return true;
}

bool FLlmShotPlanProvider::ExtractReplyText(ECineShotPlanBackend Backend, const FString& Body,
	FString& OutText, FText& OutError)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FText::Format(LOCTEXT("LlmReplyNotJson", "The backend's reply was not JSON: {0}"),
			FText::FromString(CineLlm::Summarize(Body)));
		return false;
	}

	// Every family reports errors as an "error" object even on a 200 in some cases.
	const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
	if (Root->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject->IsValid())
	{
		FString Message;
		(*ErrorObject)->TryGetStringField(TEXT("message"), Message);
		OutError = FText::Format(LOCTEXT("LlmApiError", "The backend refused the request: {0}"),
			FText::FromString(Message.IsEmpty() ? CineLlm::Summarize(Body) : Message));
		return false;
	}

	if (Backend == ECineShotPlanBackend::Anthropic)
	{
		// Content is a block list; thinking blocks may precede the text block.
		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (Root->TryGetArrayField(TEXT("content"), Blocks))
		{
			for (const TSharedPtr<FJsonValue>& Block : *Blocks)
			{
				const TSharedPtr<FJsonObject>* BlockObject = nullptr;
				if (!Block.IsValid() || !Block->TryGetObject(BlockObject))
				{
					continue;
				}

				FString Type;
				(*BlockObject)->TryGetStringField(TEXT("type"), Type);
				if (Type == TEXT("text") && (*BlockObject)->TryGetStringField(TEXT("text"), OutText))
				{
					return true;
				}
			}
		}

		FString StopReason;
		Root->TryGetStringField(TEXT("stop_reason"), StopReason);
		if (StopReason == TEXT("refusal"))
		{
			OutError = LOCTEXT("LlmRefusal", "The model declined to answer this request.");
			return false;
		}
		if (StopReason == TEXT("max_tokens"))
		{
			OutError = LOCTEXT("LlmTruncated",
				"The reply hit the token ceiling before the plan was finished. Raise Max Output Tokens in the CineDirector settings.");
			return false;
		}
	}
	else if (CineLlm::IsOpenAIStyle(Backend))
	{
		const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
		if (Root->TryGetArrayField(TEXT("choices"), Choices) && Choices->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Choice = nullptr;
			if ((*Choices)[0]->TryGetObject(Choice))
			{
				const TSharedPtr<FJsonObject>* Message = nullptr;
				if ((*Choice)->TryGetObjectField(TEXT("message"), Message)
					&& (*Message)->TryGetStringField(TEXT("content"), OutText)
					&& !OutText.IsEmpty())
				{
					return true;
				}
			}
		}
	}
	else // Gemini
	{
		const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
		if (Root->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			if ((*Candidates)[0]->TryGetObject(Candidate))
			{
				const TSharedPtr<FJsonObject>* Content = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
				if ((*Candidate)->TryGetObjectField(TEXT("content"), Content)
					&& (*Content)->TryGetArrayField(TEXT("parts"), Parts))
				{
					// Concatenate parts: long replies are sometimes split across them.
					for (const TSharedPtr<FJsonValue>& Part : *Parts)
					{
						const TSharedPtr<FJsonObject>* PartObject = nullptr;
						FString Text;
						if (Part->TryGetObject(PartObject) && (*PartObject)->TryGetStringField(TEXT("text"), Text))
						{
							OutText += Text;
						}
					}
					if (!OutText.IsEmpty())
					{
						return true;
					}
				}
			}
		}
	}

	OutError = FText::Format(LOCTEXT("LlmNoReplyText", "The backend's reply held no text: {0}"),
		FText::FromString(CineLlm::Summarize(Body)));
	return false;
}

void FLlmShotPlanProvider::FailOrFallBack(const FText& Error, const FString& Description,
	const FCineSceneContext& Scene, const FCineShotPlanReady& OnReady) const
{
	const UCineDirectorSettings* Settings = GetDefault<UCineDirectorSettings>();

	UE_LOG(LogCineDirectorLlm, Warning, TEXT("Shot plan request failed: %s"), *Error.ToString());

	if (Settings->bFallBackToGrammar && Fallback.IsValid())
	{
		FCineShotPlan Plan;
		FText FallbackError;
		if (Fallback->BuildShotPlan(Description, Scene, Plan, FallbackError) && Plan.Segments.Num() > 0)
		{
			// Say why the offline parser answered, or the shots look silently dumber.
			Plan.Segments[0].ParseNotes.Insert(
				FString::Printf(TEXT("Used the offline grammar parser — %s"), *Error.ToString()), 0);
			OnReady.ExecuteIfBound(true, Plan, FText::GetEmpty());
			return;
		}
	}

	OnReady.ExecuteIfBound(false, FCineShotPlan(), Error);
}

void FLlmShotPlanProvider::BuildShotPlanAsync(const FString& Description, const FCineSceneContext& Scene,
	FCineShotPlanReady OnReady)
{
	const UCineDirectorSettings* Settings = GetDefault<UCineDirectorSettings>();

	if (Description.TrimStartAndEnd().IsEmpty())
	{
		OnReady.ExecuteIfBound(false, FCineShotPlan(),
			LOCTEXT("LlmEmptyPrompt", "Describe the shots you want first."));
		return;
	}

	const FString ApiKey = FCineLlmCredentials::Resolve(*Settings);
	if (ApiKey.IsEmpty() && !CineLlm::IsKeyOptional(Settings->Backend))
	{
		FailOrFallBack(FText::Format(LOCTEXT("LlmNoKey", "No API key for {0}. {1}"),
			CineLlm::BackendDisplayName(Settings->Backend),
			FText::FromString(FCineLlmCredentials::DescribeSources(*Settings))),
			Description, Scene, OnReady);
		return;
	}

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();

	FText BuildError;
	if (!BuildRequest(*Settings, ApiKey, Description, Scene, Request, BuildError))
	{
		FailOrFallBack(BuildError, Description, Scene, OnReady);
		return;
	}

	// The scene snapshot and prompt are copied into the callback: the level may
	// have moved on by the time the reply lands, and the plan is resolved against
	// what the model was actually shown.
	TWeakPtr<FLlmShotPlanProvider> WeakThis = AsShared();
	const ECineShotPlanBackend Backend = Settings->Backend;
	const bool bLogTraffic = Settings->bLogTraffic;

	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, Backend, bLogTraffic, Description, Scene, OnReady]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			const TSharedPtr<FLlmShotPlanProvider> Provider = WeakThis.Pin();
			if (!Provider.IsValid())
			{
				return;
			}

			if (!bConnectedSuccessfully || !Response.IsValid())
			{
				Provider->FailOrFallBack(LOCTEXT("LlmNoConnection",
					"Could not reach the model backend. Check the endpoint URL and the network connection."),
					Description, Scene, OnReady);
				return;
			}

			const FString Body = Response->GetContentAsString();
			if (bLogTraffic)
			{
				UE_LOG(LogCineDirectorLlm, Log, TEXT("HTTP %d\n%s"), Response->GetResponseCode(), *Body);
			}

			const int32 Code = Response->GetResponseCode();
			if (Code < 200 || Code > 299)
			{
				Provider->FailOrFallBack(FText::Format(LOCTEXT("LlmHttpError", "Backend returned HTTP {0}: {1}"),
					FText::AsNumber(Code), FText::FromString(CineLlm::Summarize(Body))),
					Description, Scene, OnReady);
				return;
			}

			FString ReplyText;
			FText Error;
			if (!ExtractReplyText(Backend, Body, ReplyText, Error))
			{
				Provider->FailOrFallBack(Error, Description, Scene, OnReady);
				return;
			}

			FCineShotPlan Plan;
			if (!FCineShotPlanJson::ParsePlan(ReplyText, Scene, Plan, Error))
			{
				Provider->FailOrFallBack(Error, Description, Scene, OnReady);
				return;
			}

			OnReady.ExecuteIfBound(true, Plan, FText::GetEmpty());
		});

	if (!Request->ProcessRequest())
	{
		FailOrFallBack(LOCTEXT("LlmSendFailed", "The HTTP request could not be started."),
			Description, Scene, OnReady);
	}
}

FShotPlanProviderRouter::FShotPlanProviderRouter()
	: Grammar(MakeShared<FShotGrammarParser>())
	, Llm(MakeShared<FLlmShotPlanProvider>(Grammar))
{
}

IShotPlanProvider& FShotPlanProviderRouter::Active() const
{
	return GetDefault<UCineDirectorSettings>()->Backend == ECineShotPlanBackend::Offline
		? static_cast<IShotPlanProvider&>(Grammar.Get())
		: static_cast<IShotPlanProvider&>(Llm.Get());
}

FText FShotPlanProviderRouter::GetProviderName() const
{
	return Active().GetProviderName();
}

bool FShotPlanProviderRouter::IsAsynchronous() const
{
	return Active().IsAsynchronous();
}

bool FShotPlanProviderRouter::BuildShotPlan(const FString& Description, const FCineSceneContext& Scene,
	FCineShotPlan& OutPlan, FText& OutError)
{
	return Active().BuildShotPlan(Description, Scene, OutPlan, OutError);
}

void FShotPlanProviderRouter::BuildShotPlanAsync(const FString& Description, const FCineSceneContext& Scene,
	FCineShotPlanReady OnReady)
{
	Active().BuildShotPlanAsync(Description, Scene, OnReady);
}

#undef LOCTEXT_NAMESPACE
