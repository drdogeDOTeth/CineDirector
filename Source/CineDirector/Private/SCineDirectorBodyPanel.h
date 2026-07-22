// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/SCompoundWidget.h"

class AActor;
class SEditableTextBox;
class STextBlock;
class USkeletalMesh;

/**
 * Body Performance section: pick a character, describe the performance in
 * plain language ("sitting and smoking nervously, looking around"), and a
 * from-scratch keyframed body animation is baked and layered into the open
 * Level Sequence — composable with Face & Lipsync output on the same actor.
 */
class SCineDirectorBodyPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCineDirectorBodyPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnUseSelectedActor();
	FReply OnGenerate();

	USkeletalMesh* GetTargetMesh(FString& OutRigError) const;
	void SetStatus(const FString& Message, bool bIsError = false);

	TWeakObjectPtr<AActor> TargetActor;
	TSharedPtr<STextBlock> TargetLabel;
	TSharedPtr<SEditableTextBox> DescriptionBox;
	TSharedPtr<STextBlock> StatusBlock;
};
