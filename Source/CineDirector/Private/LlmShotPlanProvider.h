// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CineDirectorSettings.h"
#include "IShotPlanProvider.h"
#include "Interfaces/IHttpRequest.h"

/**
 * IShotPlanProvider backed by a hosted or local language model.
 *
 * One provider covers every backend in ECineShotPlanBackend: the plan schema,
 * prompt and parser are shared (FCineShotPlanJson), and only the request body,
 * auth header and reply shape differ per family — Anthropic messages,
 * OpenAI-style chat completions (which also covers OpenRouter, Ollama, LM Studio
 * and anything else that speaks it), and Gemini generateContent.
 *
 * The request is fired on the game thread and completed on it too, so the
 * callback may resolve actors and touch the editor directly. When the request
 * fails and the setting allows it, the offline grammar parser answers instead so
 * a dropped network connection never blocks the panel.
 */
class FLlmShotPlanProvider : public IShotPlanProvider, public TSharedFromThis<FLlmShotPlanProvider>
{
public:
	/** @param InFallback offline provider used when a request fails; may be null. */
	explicit FLlmShotPlanProvider(TSharedPtr<IShotPlanProvider> InFallback);

	virtual FText GetProviderName() const override;
	virtual bool IsAsynchronous() const override { return true; }
	virtual void BuildShotPlanAsync(const FString& Description, const FCineSceneContext& Scene, FCineShotPlanReady OnReady) override;

private:
	/** Fill in URL, headers and body for the configured backend. */
	bool BuildRequest(const UCineDirectorSettings& Settings, const FString& ApiKey,
		const FString& Description, const FCineSceneContext& Scene,
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, FText& OutError) const;

	/** Pull the assistant's text out of a backend-specific reply body. */
	static bool ExtractReplyText(ECineShotPlanBackend Backend, const FString& Body,
		FString& OutText, FText& OutError);

	/**
	 * Report a failure: either hands off to the offline parser (adding a note that
	 * says why) or surfaces the error, depending on bFallBackToGrammar.
	 */
	void FailOrFallBack(const FText& Error, const FString& Description,
		const FCineSceneContext& Scene, const FCineShotPlanReady& OnReady) const;

	TSharedPtr<IShotPlanProvider> Fallback;
};

/**
 * The provider the panel actually holds: forwards each request to the offline
 * grammar parser or the model backend according to the current setting, so
 * switching backends in Project Settings takes effect on the next click rather
 * than on the next editor restart.
 */
class FShotPlanProviderRouter : public IShotPlanProvider
{
public:
	FShotPlanProviderRouter();

	virtual FText GetProviderName() const override;
	virtual bool IsAsynchronous() const override;
	virtual bool BuildShotPlan(const FString& Description, const FCineSceneContext& Scene, FCineShotPlan& OutPlan, FText& OutError) override;
	virtual void BuildShotPlanAsync(const FString& Description, const FCineSceneContext& Scene, FCineShotPlanReady OnReady) override;

private:
	IShotPlanProvider& Active() const;

	TSharedRef<IShotPlanProvider> Grammar;
	TSharedRef<FLlmShotPlanProvider> Llm;
};
