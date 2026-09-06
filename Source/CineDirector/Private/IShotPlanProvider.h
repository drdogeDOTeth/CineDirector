// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShotPlanTypes.h"

/**
 * Result of an asynchronous plan request. Always fired on the game thread, so
 * the handler may touch actors, Slate and the editor world directly.
 */
DECLARE_DELEGATE_ThreeParams(FCineShotPlanReady, bool /*bSuccess*/, const FCineShotPlan& /*Plan*/, const FText& /*Error*/);

/**
 * Turns a natural-language shot description into an executable FCineShotPlan.
 *
 * Implementations:
 *  - FShotGrammarParser: built-in rule-based vocabulary, offline, deterministic.
 *  - FLlmShotPlanProvider: sends the description plus FCineSceneContext to a chat
 *    completions endpoint (Anthropic, OpenAI-compatible or Gemini) and deserializes
 *    the returned plan. Anything that can fill in an FCineShotPlan from a string
 *    can be dropped in here.
 *
 * A provider implements whichever of the two entry points it can answer:
 * synchronous providers override BuildShotPlan and get BuildShotPlanAsync for
 * free; providers that have to wait on I/O override BuildShotPlanAsync. Callers
 * should prefer BuildShotPlanAsync — it covers both.
 */
class IShotPlanProvider
{
public:
	virtual ~IShotPlanProvider() = default;

	/** Display name shown in the panel's provider picker. */
	virtual FText GetProviderName() const = 0;

	/** True when a request may take long enough that the UI should show progress. */
	virtual bool IsAsynchronous() const { return false; }

	/**
	 * Build a shot plan from a description, blocking until done.
	 * @return false with OutError set if nothing usable could be interpreted.
	 *
	 * The default refuses: asynchronous providers cannot answer inline.
	 */
	virtual bool BuildShotPlan(const FString& Description, const FCineSceneContext& Scene, FCineShotPlan& OutPlan, FText& OutError)
	{
		OutError = NSLOCTEXT("CineDirector", "ProviderIsAsync",
			"This shot-plan provider only answers asynchronously.");
		return false;
	}

	/**
	 * Build a shot plan and report it through OnReady. The default runs the
	 * synchronous path and completes immediately, so a caller never has to
	 * branch on IsAsynchronous().
	 */
	virtual void BuildShotPlanAsync(const FString& Description, const FCineSceneContext& Scene, FCineShotPlanReady OnReady)
	{
		FCineShotPlan Plan;
		FText Error;
		const bool bSuccess = BuildShotPlan(Description, Scene, Plan, Error);
		OnReady.ExecuteIfBound(bSuccess, Plan, Error);
	}
};
