// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShotPlanTypes.h"

/**
 * The wire format between any language model and FCineShotPlan.
 *
 * One JSON Schema describes a plan; the same schema is handed to the model as a
 * structured-output constraint (Anthropic output_config.format, OpenAI
 * response_format, Gemini responseSchema) and used here to read the reply back.
 * Keeping schema, prompt and parser in one file means adding a segment field is
 * a single edit in three adjacent functions.
 *
 * The schema is deliberately strict — every property required, no additional
 * properties — because that is what the strict modes of all three families
 * demand. Neutral defaults ("static", 0, false) stand in for "not applicable",
 * and the field descriptions tell the model so.
 */
struct FCineShotPlanJson
{
	/** JSON Schema for one plan, as a compact object literal. */
	static FString BuildSchema();

	/** Directing instructions + vocabulary, sent as the system prompt. */
	static FString BuildSystemPrompt();

	/**
	 * The user turn: the description plus a snapshot of what is in the level, so
	 * the model can name subjects. Scene actors are omitted when bSendSceneActors
	 * is false; MaxActors keeps the nearest ones to the viewport.
	 */
	static FString BuildUserPrompt(const FString& Description, const FCineSceneContext& Scene,
		bool bSendSceneActors, int32 MaxActors);

	/**
	 * Read a model reply into a plan, resolving actor labels against the level.
	 * Tolerates a reply wrapped in prose or a ```json fence, which local models
	 * still produce even when asked for raw JSON.
	 *
	 * @return false with OutError set when the reply had no usable plan in it.
	 */
	static bool ParsePlan(const FString& Reply, const FCineSceneContext& Scene,
		FCineShotPlan& OutPlan, FText& OutError);

	/** First balanced {...} run in a string, for replies that are not bare JSON. */
	static bool ExtractJsonObject(const FString& Text, FString& OutJson);

	/** Readable one-line-per-shot dump of a plan, for logs and headless checks. */
	static FString DescribePlan(const FCineShotPlan& Plan);
};
