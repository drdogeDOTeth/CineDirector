// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShotPlanTypes.h"

/**
 * Turns a natural-language shot description into an executable FCineShotPlan.
 *
 * Implementations:
 *  - FShotGrammarParser: built-in rule-based vocabulary, offline, deterministic.
 *  - (future) FClaudeShotPlanProvider: sends the description plus FCineSceneContext to the
 *    Claude API and deserializes the returned plan. Anything that can fill in an
 *    FCineShotPlan from a string can be dropped in here.
 */
class IShotPlanProvider
{
public:
	virtual ~IShotPlanProvider() = default;

	/** Display name shown in the panel's provider picker. */
	virtual FText GetProviderName() const = 0;

	/**
	 * Build a shot plan from a description.
	 * @return false with OutError set if nothing usable could be interpreted.
	 */
	virtual bool BuildShotPlan(const FString& Description, const FCineSceneContext& Scene, FCineShotPlan& OutPlan, FText& OutError) = 0;
};
