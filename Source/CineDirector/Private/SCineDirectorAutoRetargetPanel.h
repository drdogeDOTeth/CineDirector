// Copyright Roundtree. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "CineDirectorAutoRetargetTypes.h"

class SEditableTextBox;
class STextBlock;
class SVerticalBox;
class USkeletalMesh;

/** CineDirector's interactive Skeletal Mesh to IK Rig workflow. */
class SCineDirectorAutoRetargetPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCineDirectorAutoRetargetPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	struct FMappingEditors
	{
		int32 MappingIndex = INDEX_NONE;
		TSharedPtr<SEditableTextBox> Start;
		TSharedPtr<SEditableTextBox> End;
	};

	FReply OnUseSelectedMesh();
	FReply OnAnalyze();
	FReply OnGenerate();
	FReply OnSaveProfile();
	FReply OnLoadSelectedProfile();

	USkeletalMesh* FindSelectedSkeletalMesh() const;
	void SyncEditsToAnalysis();
	void RefreshMappingTable();
	void SetStatus(const FText& Message, bool bIsError = false);
	static FText RigTypeText(ECineDirectorRigType RigType);

	TWeakObjectPtr<USkeletalMesh> SelectedMesh;
	FCineDirectorSkeletonAnalysis Analysis;
	TArray<FMappingEditors> MappingEditors;

	TSharedPtr<STextBlock> SelectedMeshText;
	TSharedPtr<STextBlock> RigSummaryText;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SVerticalBox> MappingBox;
	TSharedPtr<SEditableTextBox> OutputPathBox;
};
