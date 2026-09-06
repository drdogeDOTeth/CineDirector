// Copyright Roundtree. All Rights Reserved.

#include "CineDirectorSettings.h"

#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Short stable token for a backend, used for the key filename. */
	FString BackendSlug(ECineShotPlanBackend Backend)
	{
		switch (Backend)
		{
		case ECineShotPlanBackend::Anthropic:              return TEXT("Anthropic");
		case ECineShotPlanBackend::OpenAI:                 return TEXT("OpenAI");
		case ECineShotPlanBackend::OpenRouter:             return TEXT("OpenRouter");
		case ECineShotPlanBackend::LocalOpenAICompatible:  return TEXT("Local");
		case ECineShotPlanBackend::GoogleGemini:           return TEXT("Gemini");
		case ECineShotPlanBackend::CustomOpenAICompatible: return TEXT("Custom");
		default:                                           return TEXT("Offline");
		}
	}

	/** Environment variables checked for this backend, most specific first. */
	TArray<FString> EnvVarsFor(ECineShotPlanBackend Backend)
	{
		TArray<FString> Vars;
		switch (Backend)
		{
		case ECineShotPlanBackend::Anthropic:
			Vars.Add(TEXT("ANTHROPIC_API_KEY"));
			break;
		case ECineShotPlanBackend::OpenAI:
			Vars.Add(TEXT("OPENAI_API_KEY"));
			break;
		case ECineShotPlanBackend::OpenRouter:
			Vars.Add(TEXT("OPENROUTER_API_KEY"));
			break;
		case ECineShotPlanBackend::GoogleGemini:
			Vars.Add(TEXT("GEMINI_API_KEY"));
			Vars.Add(TEXT("GOOGLE_API_KEY"));
			break;
		default:
			break;
		}
		Vars.Add(TEXT("CINEDIRECTOR_API_KEY"));
		return Vars;
	}

	FString KeyFilePath(ECineShotPlanBackend Backend)
	{
		// Absolute: this path is shown to the user in the "no key" error, and
		// ProjectSavedDir() is relative to the engine binary.
		return FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("CineDirector") / (BackendSlug(Backend) + TEXT(".key")));
	}
}

UCineDirectorSettings::UCineDirectorSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("CineDirector");
}

#if WITH_EDITOR
void UCineDirectorSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	SaveConfig();
}
#endif

FString UCineDirectorSettings::ResolveEndpoint() const
{
	if (!EndpointOverride.IsEmpty())
	{
		return EndpointOverride;
	}

	switch (Backend)
	{
	case ECineShotPlanBackend::Anthropic:
		return TEXT("https://api.anthropic.com/v1/messages");
	case ECineShotPlanBackend::OpenAI:
		return TEXT("https://api.openai.com/v1/chat/completions");
	case ECineShotPlanBackend::OpenRouter:
		return TEXT("https://openrouter.ai/api/v1/chat/completions");
	case ECineShotPlanBackend::LocalOpenAICompatible:
		// Ollama's OpenAI-compatible shim; LM Studio defaults to :1234 instead.
		return TEXT("http://localhost:11434/v1/chat/completions");
	case ECineShotPlanBackend::GoogleGemini:
		// The model id is spliced into the path by the Gemini request builder.
		return TEXT("https://generativelanguage.googleapis.com/v1beta/models");
	default:
		return FString();
	}
}

FString UCineDirectorSettings::ResolveModel() const
{
	if (!Model.IsEmpty())
	{
		return Model;
	}

	switch (Backend)
	{
	case ECineShotPlanBackend::Anthropic:
		return TEXT("claude-opus-5");
	case ECineShotPlanBackend::OpenAI:
		return TEXT("gpt-4o");
	case ECineShotPlanBackend::GoogleGemini:
		return TEXT("gemini-2.5-pro");
	default:
		// OpenRouter / local / custom catalogs are user-specific: no safe default.
		return FString();
	}
}

FString FCineLlmCredentials::Resolve(const UCineDirectorSettings& Settings)
{
	if (!Settings.ApiKey.IsEmpty())
	{
		return Settings.ApiKey.TrimStartAndEnd();
	}

	for (const FString& Var : EnvVarsFor(Settings.Backend))
	{
		const FString Value = FPlatformMisc::GetEnvironmentVariable(*Var);
		if (!Value.IsEmpty())
		{
			return Value.TrimStartAndEnd();
		}
	}

	FString FromFile;
	if (FFileHelper::LoadFileToString(FromFile, *KeyFilePath(Settings.Backend)))
	{
		return FromFile.TrimStartAndEnd();
	}

	return FString();
}

FString FCineLlmCredentials::DescribeSources(const UCineDirectorSettings& Settings)
{
	FString Vars;
	for (const FString& Var : EnvVarsFor(Settings.Backend))
	{
		Vars += (Vars.IsEmpty() ? TEXT("") : TEXT(" or ")) + Var;
	}

	return FString::Printf(
		TEXT("Set it in Project Settings → Plugins → CineDirector, or in the %s environment variable, or in %s."),
		*Vars, *KeyFilePath(Settings.Backend));
}
