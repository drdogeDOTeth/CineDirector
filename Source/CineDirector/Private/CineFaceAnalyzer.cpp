// Copyright Roundtree. All Rights Reserved.

#include "CineFaceAnalyzer.h"

#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirectorFace, Log, All);

const TCHAR* CineFaceSlotName(ECineFaceSlot Slot)
{
	switch (Slot)
	{
	case ECineFaceSlot::JawOpen:     return TEXT("JawOpen");
	case ECineFaceSlot::MouthClose:  return TEXT("MouthClose");
	case ECineFaceSlot::MouthWide:   return TEXT("MouthWide");
	case ECineFaceSlot::MouthPucker: return TEXT("MouthPucker");
	case ECineFaceSlot::MouthFunnel: return TEXT("MouthFunnel");
	case ECineFaceSlot::MouthSmile:  return TEXT("MouthSmile");
	case ECineFaceSlot::MouthFrown:  return TEXT("MouthFrown");
	case ECineFaceSlot::MouthPress:  return TEXT("MouthPress");
	case ECineFaceSlot::NoseSneer:   return TEXT("NoseSneer");
	case ECineFaceSlot::BrowUp:      return TEXT("BrowUp");
	case ECineFaceSlot::BrowDown:    return TEXT("BrowDown");
	case ECineFaceSlot::BrowSad:     return TEXT("BrowSad");
	case ECineFaceSlot::EyeBlink:    return TEXT("EyeBlink");
	case ECineFaceSlot::EyeWide:     return TEXT("EyeWide");
	case ECineFaceSlot::EyeSquint:   return TEXT("EyeSquint");
	default:                         return TEXT("Unknown");
	}
}

namespace
{
	/** Lowercase with every non-alphanumeric stripped, so Mouth_Smile_L == mouthsmilel. */
	FString NormalizeName(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (TCHAR C : In)
		{
			if (FChar::IsAlnum(C))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
		}
		return Out;
	}

	struct FNamePattern
	{
		const TCHAR* Contains;
		ECineFaceSlot Slot;
		float Scale;
	};

	/**
	 * Exact (normalized) matches first: ARKit's 52, Oculus visemes, and the
	 * Reallusion/CC viseme set — the names audio-to-face pipelines emit.
	 */
	const TMap<FString, TPair<ECineFaceSlot, float>>& ExactTable()
	{
		static TMap<FString, TPair<ECineFaceSlot, float>> Table;
		if (Table.Num() == 0)
		{
			auto Add = [](const TCHAR* Name, ECineFaceSlot Slot, float Scale = 1.0f)
			{
				Table.Add(NormalizeName(Name), TPair<ECineFaceSlot, float>(Slot, Scale));
			};
			// ARKit
			Add(TEXT("jawOpen"), ECineFaceSlot::JawOpen);
			Add(TEXT("mouthClose"), ECineFaceSlot::MouthClose);
			Add(TEXT("mouthPucker"), ECineFaceSlot::MouthPucker);
			Add(TEXT("mouthFunnel"), ECineFaceSlot::MouthFunnel);
			Add(TEXT("mouthSmileLeft"), ECineFaceSlot::MouthSmile);
			Add(TEXT("mouthSmileRight"), ECineFaceSlot::MouthSmile);
			Add(TEXT("mouthFrownLeft"), ECineFaceSlot::MouthFrown);
			Add(TEXT("mouthFrownRight"), ECineFaceSlot::MouthFrown);
			Add(TEXT("mouthPressLeft"), ECineFaceSlot::MouthPress);
			Add(TEXT("mouthPressRight"), ECineFaceSlot::MouthPress);
			Add(TEXT("mouthStretchLeft"), ECineFaceSlot::MouthWide);
			Add(TEXT("mouthStretchRight"), ECineFaceSlot::MouthWide);
			Add(TEXT("noseSneerLeft"), ECineFaceSlot::NoseSneer);
			Add(TEXT("noseSneerRight"), ECineFaceSlot::NoseSneer);
			Add(TEXT("browInnerUp"), ECineFaceSlot::BrowSad);
			Add(TEXT("browOuterUpLeft"), ECineFaceSlot::BrowUp);
			Add(TEXT("browOuterUpRight"), ECineFaceSlot::BrowUp);
			Add(TEXT("browDownLeft"), ECineFaceSlot::BrowDown);
			Add(TEXT("browDownRight"), ECineFaceSlot::BrowDown);
			Add(TEXT("eyeBlinkLeft"), ECineFaceSlot::EyeBlink);
			Add(TEXT("eyeBlinkRight"), ECineFaceSlot::EyeBlink);
			Add(TEXT("eyeWideLeft"), ECineFaceSlot::EyeWide);
			Add(TEXT("eyeWideRight"), ECineFaceSlot::EyeWide);
			Add(TEXT("eyeSquintLeft"), ECineFaceSlot::EyeSquint);
			Add(TEXT("eyeSquintRight"), ECineFaceSlot::EyeSquint);
			// Oculus visemes
			Add(TEXT("viseme_aa"), ECineFaceSlot::JawOpen);
			Add(TEXT("viseme_E"), ECineFaceSlot::MouthWide);
			Add(TEXT("viseme_ih"), ECineFaceSlot::MouthWide, 0.7f);
			Add(TEXT("viseme_oh"), ECineFaceSlot::MouthFunnel);
			Add(TEXT("viseme_ou"), ECineFaceSlot::MouthPucker);
			Add(TEXT("viseme_PP"), ECineFaceSlot::MouthClose);
			Add(TEXT("viseme_SS"), ECineFaceSlot::MouthWide, 0.5f);
			Add(TEXT("viseme_FF"), ECineFaceSlot::MouthPress, 0.6f);
			// Reallusion / Character Creator
			Add(TEXT("V_Open"), ECineFaceSlot::JawOpen);
			Add(TEXT("V_Wide"), ECineFaceSlot::MouthWide);
			Add(TEXT("V_Tight_O"), ECineFaceSlot::MouthPucker);
			Add(TEXT("V_Explosive"), ECineFaceSlot::MouthClose);
			Add(TEXT("V_Lip_Open"), ECineFaceSlot::MouthFunnel, 0.7f);
			Add(TEXT("V_Dental_Lip"), ECineFaceSlot::MouthPress, 0.6f);
		}
		return Table;
	}

	/** Fuzzy fallbacks, tested in order — first hit wins for a given morph. */
	const FNamePattern* FuzzyTable(int32& OutNum)
	{
		static const FNamePattern Patterns[] = {
			{ TEXT("jawopen"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("mouthopen"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("jawdrop"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("openmouth"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("mouthah"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("aa"), ECineFaceSlot::JawOpen, 1.0f },
			{ TEXT("blink"), ECineFaceSlot::EyeBlink, 1.0f },
			{ TEXT("eyesclosed"), ECineFaceSlot::EyeBlink, 1.0f },
			{ TEXT("eyeclose"), ECineFaceSlot::EyeBlink, 1.0f },
			{ TEXT("eyewide"), ECineFaceSlot::EyeWide, 1.0f },
			{ TEXT("squint"), ECineFaceSlot::EyeSquint, 1.0f },
			{ TEXT("smile"), ECineFaceSlot::MouthSmile, 1.0f },
			{ TEXT("happy"), ECineFaceSlot::MouthSmile, 0.8f },
			{ TEXT("frown"), ECineFaceSlot::MouthFrown, 1.0f },
			{ TEXT("sadmouth"), ECineFaceSlot::MouthFrown, 1.0f },
			{ TEXT("pucker"), ECineFaceSlot::MouthPucker, 1.0f },
			{ TEXT("kiss"), ECineFaceSlot::MouthPucker, 1.0f },
			{ TEXT("funnel"), ECineFaceSlot::MouthFunnel, 1.0f },
			{ TEXT("mouthwide"), ECineFaceSlot::MouthWide, 1.0f },
			{ TEXT("stretch"), ECineFaceSlot::MouthWide, 0.8f },
			{ TEXT("mouthpress"), ECineFaceSlot::MouthPress, 1.0f },
			{ TEXT("lipspressed"), ECineFaceSlot::MouthPress, 1.0f },
			{ TEXT("mouthclose"), ECineFaceSlot::MouthClose, 1.0f },
			{ TEXT("sneer"), ECineFaceSlot::NoseSneer, 1.0f },
			{ TEXT("snarl"), ECineFaceSlot::NoseSneer, 1.0f },
			{ TEXT("browsup"), ECineFaceSlot::BrowUp, 1.0f },
			{ TEXT("browup"), ECineFaceSlot::BrowUp, 1.0f },
			{ TEXT("browraise"), ECineFaceSlot::BrowUp, 1.0f },
			{ TEXT("browsdown"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("browdown"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("browfurrow"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("angrybrow"), ECineFaceSlot::BrowDown, 1.0f },
			{ TEXT("browinnerup"), ECineFaceSlot::BrowSad, 1.0f },
			{ TEXT("worried"), ECineFaceSlot::BrowSad, 0.8f },
		};
		OutNum = UE_ARRAY_COUNT(Patterns);
		return Patterns;
	}

	/** MetaHuman rig-logic control curves for each slot (left/right where split). */
	void FillMetaHumanProfile(FCineFaceProfile& Profile)
	{
		auto Bind = [&Profile](ECineFaceSlot Slot, std::initializer_list<const TCHAR*> Curves, float Scale = 1.0f)
		{
			for (const TCHAR* Curve : Curves)
			{
				Profile.Slots[(int32)Slot].Add({ FName(Curve), Scale });
			}
		};
		Bind(ECineFaceSlot::JawOpen, { TEXT("CTRL_expressions_jawOpen") });
		Bind(ECineFaceSlot::MouthClose, { TEXT("CTRL_expressions_mouthLipsTogetherUL"), TEXT("CTRL_expressions_mouthLipsTogetherUR"), TEXT("CTRL_expressions_mouthLipsTogetherDL"), TEXT("CTRL_expressions_mouthLipsTogetherDR") });
		Bind(ECineFaceSlot::MouthWide, { TEXT("CTRL_expressions_mouthStretchL"), TEXT("CTRL_expressions_mouthStretchR") }, 0.8f);
		Bind(ECineFaceSlot::MouthPucker, { TEXT("CTRL_expressions_mouthLipsPurseUL"), TEXT("CTRL_expressions_mouthLipsPurseUR"), TEXT("CTRL_expressions_mouthLipsPurseDL"), TEXT("CTRL_expressions_mouthLipsPurseDR") });
		Bind(ECineFaceSlot::MouthFunnel, { TEXT("CTRL_expressions_mouthLipsFunnelUL"), TEXT("CTRL_expressions_mouthLipsFunnelUR"), TEXT("CTRL_expressions_mouthLipsFunnelDL"), TEXT("CTRL_expressions_mouthLipsFunnelDR") });
		Bind(ECineFaceSlot::MouthSmile, { TEXT("CTRL_expressions_mouthCornerPullL"), TEXT("CTRL_expressions_mouthCornerPullR") });
		Bind(ECineFaceSlot::MouthFrown, { TEXT("CTRL_expressions_mouthCornerDepressL"), TEXT("CTRL_expressions_mouthCornerDepressR") });
		Bind(ECineFaceSlot::MouthPress, { TEXT("CTRL_expressions_mouthPressUL"), TEXT("CTRL_expressions_mouthPressUR"), TEXT("CTRL_expressions_mouthPressDL"), TEXT("CTRL_expressions_mouthPressDR") });
		Bind(ECineFaceSlot::NoseSneer, { TEXT("CTRL_expressions_noseWrinkleL"), TEXT("CTRL_expressions_noseWrinkleR") });
		Bind(ECineFaceSlot::BrowUp, { TEXT("CTRL_expressions_browRaiseOuterL"), TEXT("CTRL_expressions_browRaiseOuterR") });
		Bind(ECineFaceSlot::BrowDown, { TEXT("CTRL_expressions_browDownL"), TEXT("CTRL_expressions_browDownR") });
		Bind(ECineFaceSlot::BrowSad, { TEXT("CTRL_expressions_browRaiseInL"), TEXT("CTRL_expressions_browRaiseInR") });
		Bind(ECineFaceSlot::EyeBlink, { TEXT("CTRL_expressions_eyeBlinkL"), TEXT("CTRL_expressions_eyeBlinkR") });
		Bind(ECineFaceSlot::EyeWide, { TEXT("CTRL_expressions_eyeWidenL"), TEXT("CTRL_expressions_eyeWidenR") });
		Bind(ECineFaceSlot::EyeSquint, { TEXT("CTRL_expressions_eyeSquintInnerL"), TEXT("CTRL_expressions_eyeSquintInnerR") });
	}
}

FCineFaceProfile FCineFaceAnalyzer::Analyze(USkeletalMesh* Mesh)
{
	FCineFaceProfile Profile;
	if (!Mesh)
	{
		Profile.Notes.Add(TEXT("No skeletal mesh."));
		return Profile;
	}
	Profile.MeshName = Mesh->GetName();

	// MetaHuman faces animate through rig-logic control curves, not raw morphs.
	const USkeleton* Skeleton = Mesh->GetSkeleton();
	bool bLooksMetaHuman = Skeleton && Skeleton->GetName().Contains(TEXT("Face_Archetype"));
	if (!bLooksMetaHuman && Skeleton)
	{
		Skeleton->ForEachCurveMetaData([&bLooksMetaHuman](const FName& CurveName, const FCurveMetaData&)
		{
			bLooksMetaHuman |= CurveName.ToString().StartsWith(TEXT("CTRL_expressions_"));
		});
	}
	if (bLooksMetaHuman)
	{
		Profile.bMetaHuman = true;
		FillMetaHumanProfile(Profile);
		Profile.Notes.Add(TEXT("MetaHuman face detected — driving CTRL_expressions rig controls."));
		return Profile;
	}

	const TArray<TObjectPtr<UMorphTarget>>& Morphs = Mesh->GetMorphTargets();
	if (Morphs.Num() == 0)
	{
		Profile.Notes.Add(FString::Printf(TEXT("'%s' has no morph targets — facial animation needs a face mesh with blendshapes."), *Profile.MeshName));
		return Profile;
	}

	int32 NumFuzzy = 0;
	const FNamePattern* Fuzzy = FuzzyTable(NumFuzzy);
	int32 Unrecognized = 0;

	for (const UMorphTarget* Morph : Morphs)
	{
		if (!Morph)
		{
			continue;
		}
		const FName MorphName = Morph->GetFName();
		const FString Norm = NormalizeName(MorphName.ToString());

		if (const TPair<ECineFaceSlot, float>* Exact = ExactTable().Find(Norm))
		{
			Profile.Slots[(int32)Exact->Key].Add({ MorphName, Exact->Value });
			continue;
		}

		bool bMatched = false;
		for (int32 i = 0; i < NumFuzzy && !bMatched; ++i)
		{
			if (Norm.Contains(Fuzzy[i].Contains))
			{
				Profile.Slots[(int32)Fuzzy[i].Slot].Add({ MorphName, Fuzzy[i].Scale });
				bMatched = true;
			}
		}
		Unrecognized += bMatched ? 0 : 1;
	}

	Profile.Notes.Add(FString::Printf(TEXT("%d morph targets scanned, %d unrecognized."), Morphs.Num(), Unrecognized));
	if (!Profile.HasSlot(ECineFaceSlot::JawOpen))
	{
		Profile.Notes.Add(TEXT("No jaw/mouth-open morph found — lipsync will be very subtle. Check the mesh's morph names."));
	}
	return Profile;
}

FString FCineFaceAnalyzer::DescribeProfile(const FCineFaceProfile& Profile)
{
	TArray<FString> Mapped;
	TArray<FString> Missing;
	for (int32 i = 0; i < (int32)ECineFaceSlot::Count; ++i)
	{
		if (Profile.Slots[i].Num() > 0)
		{
			Mapped.Add(FString::Printf(TEXT("%s(%d)"), CineFaceSlotName((ECineFaceSlot)i), Profile.Slots[i].Num()));
		}
		else
		{
			Missing.Add(CineFaceSlotName((ECineFaceSlot)i));
		}
	}

	FString Out = FString::Printf(TEXT("%s: mapped %d/%d face slots"),
		*Profile.MeshName, Profile.NumMappedSlots(), (int32)ECineFaceSlot::Count);
	if (Mapped.Num() > 0)
	{
		Out += FString::Printf(TEXT(" — %s"), *FString::Join(Mapped, TEXT(", ")));
	}
	if (Missing.Num() > 0 && Missing.Num() < (int32)ECineFaceSlot::Count)
	{
		Out += FString::Printf(TEXT(". Unmapped: %s"), *FString::Join(Missing, TEXT(", ")));
	}
	for (const FString& Note : Profile.Notes)
	{
		Out += TEXT("\n") + Note;
	}
	UE_LOG(LogCineDirectorFace, Log, TEXT("%s"), *Out);
	return Out;
}
