// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "CineDirectorSettings.generated.h"

/**
 * Which backend interprets the prompt. Everything except Offline talks to a
 * chat-completions style HTTP endpoint; the wire format differs per family.
 */
UENUM()
enum class ECineShotPlanBackend : uint8
{
	/** FShotGrammarParser: rule-based, offline, deterministic, no key needed. */
	Offline UMETA(DisplayName = "Offline grammar (no API)"),

	/** api.anthropic.com /v1/messages. */
	Anthropic UMETA(DisplayName = "Anthropic (Claude)"),

	/** api.openai.com /v1/chat/completions. */
	OpenAI UMETA(DisplayName = "OpenAI"),

	/** openrouter.ai — OpenAI-compatible, routes to many vendors. */
	OpenRouter UMETA(DisplayName = "OpenRouter"),

	/** Ollama / LM Studio / vLLM on localhost — OpenAI-compatible, usually keyless. */
	LocalOpenAICompatible UMETA(DisplayName = "Local model (Ollama / LM Studio)"),

	/** generativelanguage.googleapis.com :generateContent. */
	GoogleGemini UMETA(DisplayName = "Google Gemini"),

	/** Any other OpenAI-compatible endpoint (xAI, DeepSeek, Groq, Azure, a proxy…). */
	CustomOpenAICompatible UMETA(DisplayName = "Custom (OpenAI-compatible)"),
};

/** Reasoning depth hint. Only sent to backends that expose one; ignored elsewhere. */
UENUM()
enum class ECineLlmEffort : uint8
{
	Low,
	Medium,
	High,
};

/**
 * Project Settings → Plugins → CineDirector.
 *
 * Stored in EditorPerProjectUserSettings (per user, not the shared project ini)
 * so an API key typed here does not land in source control. Preferred anyway is
 * to leave ApiKey empty and set the provider's environment variable — see
 * FCineLlmCredentials::Resolve for the lookup order.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "CineDirector"))
class UCineDirectorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCineDirectorSettings();

#if WITH_EDITOR
	/** Persist edits to the per-user editor ini rather than the shared project config. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Which backend the shot panel uses. Offline needs no key and never leaves the machine. */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (DisplayName = "Backend"))
	ECineShotPlanBackend Backend = ECineShotPlanBackend::Offline;

	/**
	 * Model id, exactly as the chosen backend spells it (e.g. "claude-opus-5",
	 * "gpt-4o", "anthropic/claude-sonnet-4.5" on OpenRouter, "llama3.1" on Ollama).
	 * Leave empty to use the built-in default for the backend, where one exists.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning")
	FString Model;

	/**
	 * Full endpoint URL. Empty = the backend's default. Set this for self-hosted
	 * models, a corporate proxy, or a non-default Ollama port.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (DisplayName = "Endpoint URL override"))
	FString EndpointOverride;

	/**
	 * API key. Leave empty (recommended) and the key is read from the backend's
	 * environment variable, or from Saved/CineDirector/<Backend>.key.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (DisplayName = "API key (optional — prefer env var)", PasswordField = true))
	FString ApiKey;

	/** Reasoning depth for backends that expose it. Low is usually plenty for shot planning. */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning")
	ECineLlmEffort Effort = ECineLlmEffort::Low;

	/** Output token ceiling for one plan. Headroom matters: on thinking models this covers reasoning too. */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (ClampMin = "1024", ClampMax = "64000"))
	int32 MaxOutputTokens = 8000;

	/** Give up on the request after this long. */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (ClampMin = "5.0", ClampMax = "600.0"))
	float TimeoutSeconds = 120.0f;

	/**
	 * If the request fails (no key, network down, unparseable reply), fall back to
	 * the offline grammar parser instead of surfacing an error. The panel says so
	 * in its notes when this happens.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (DisplayName = "Fall back to offline grammar on failure"))
	bool bFallBackToGrammar = true;

	/**
	 * Include the level's actor labels, positions and bounds in the prompt so the
	 * model can pick subjects by name. Turn off if the level is huge or the names
	 * are sensitive; shots then have to be framed from the viewport.
	 */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning")
	bool bSendSceneActors = true;

	/** Cap on how many actor labels get sent, nearest to the viewport camera first. */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (ClampMin = "1", ClampMax = "500", EditCondition = "bSendSceneActors"))
	int32 MaxSceneActors = 60;

	/** Log the full request and reply bodies to the output log. Keys are never logged. */
	UPROPERTY(EditAnywhere, config, Category = "Shot Planning",
		meta = (DisplayName = "Log request / response bodies"))
	bool bLogTraffic = false;

	/** Default endpoint for the chosen backend, used when EndpointOverride is empty. */
	FString ResolveEndpoint() const;

	/** Configured model, or the backend's default when Model is empty. */
	FString ResolveModel() const;
};

/** Key lookup, kept out of the settings object so it can be reused headlessly. */
struct FCineLlmCredentials
{
	/**
	 * In order: the ApiKey setting, then the backend's environment variable
	 * (ANTHROPIC_API_KEY / OPENAI_API_KEY / OPENROUTER_API_KEY / GEMINI_API_KEY,
	 * plus CINEDIRECTOR_API_KEY as a catch-all), then
	 * <Project>/Saved/CineDirector/<Backend>.key.
	 *
	 * Returns an empty string when nothing is configured; local backends treat
	 * that as fine, everything else reports it as an error.
	 */
	static FString Resolve(const UCineDirectorSettings& Settings);

	/** Human-readable list of where a key was looked for, for the "no key" error. */
	static FString DescribeSources(const UCineDirectorSettings& Settings);
};
