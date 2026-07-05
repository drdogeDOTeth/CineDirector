// Copyright Roundtree. All Rights Reserved.

#include "ShotPlanExecutor.h"

#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "LevelEditorViewport.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "MovieScene.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "ScopedTransaction.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"

#define LOCTEXT_NAMESPACE "CineDirector"

DEFINE_LOG_CATEGORY_STATIC(LogCineDirector, Log, All);

namespace CineDirectorExec
{
	FVector ActorCenter(const AActor* Actor)
	{
		const FBox Bounds = Actor->GetComponentsBoundingBox(true);
		return Bounds.IsValid ? Bounds.GetCenter() : Actor->GetActorLocation();
	}

	/** Where the camera lives relative to the subject, before the move is applied. */
	struct FShotGeometry
	{
		bool bHasTarget = false;
		FVector TargetPoint = FVector::ZeroVector;
		double Radius = 100.0;
		double Distance = 500.0;
		/** World-space yaw of (camera - target). */
		double AzimuthDeg = 0.0;
		double ElevationDeg = 0.0;
	};

	/** Camera distance as a multiple of the subject's bounds radius. */
	double FramingFactor(ECineShotSize Size)
	{
		switch (Size)
		{
		case ECineShotSize::ExtremeCloseUp: return 1.6;
		case ECineShotSize::CloseUp:        return 2.4;
		case ECineShotSize::MediumCloseUp:  return 3.2;
		case ECineShotSize::Medium:         return 4.5;
		case ECineShotSize::Wide:           return 8.0;
		case ECineShotSize::ExtremeWide:    return 15.0;
		default:                            return 4.5;
		}
	}

	/** Unit vector at the given yaw/pitch angles (degrees), Z-up. */
	FVector SphericalOffset(double AzimuthDeg, double ElevationDeg)
	{
		const double Az = FMath::DegreesToRadians(AzimuthDeg);
		const double El = FMath::DegreesToRadians(ElevationDeg);
		return FVector(FMath::Cos(El) * FMath::Cos(Az), FMath::Cos(El) * FMath::Sin(Az), FMath::Sin(El));
	}

	FShotGeometry ComputeGeometry(const FCineShotSegment& Seg, const FVector& ViewLoc, const FRotator& ViewRot)
	{
		FShotGeometry Geo;

		if (AActor* Target = Seg.TargetActor.Get())
		{
			const FBox Bounds = Target->GetComponentsBoundingBox(true);
			Geo.bHasTarget = true;
			Geo.TargetPoint = Bounds.IsValid ? Bounds.GetCenter() : Target->GetActorLocation();
			Geo.Radius = Bounds.IsValid ? FMath::Max<double>(Bounds.GetExtent().Size(), 25.0) : 100.0;

			const double FacingYaw = Target->GetActorRotation().Yaw;
			switch (Seg.ViewSide)
			{
			case ECineViewSide::Front:        Geo.AzimuthDeg = FacingYaw; break;
			case ECineViewSide::Behind:       Geo.AzimuthDeg = FacingYaw + 180.0; break;
			case ECineViewSide::Left:         Geo.AzimuthDeg = FacingYaw - 90.0; break;
			case ECineViewSide::Right:        Geo.AzimuthDeg = FacingYaw + 90.0; break;
			case ECineViewSide::OverShoulder: Geo.AzimuthDeg = FacingYaw + 145.0; break;
			}

			switch (Seg.Angle)
			{
			case ECineAngle::Low:      Geo.ElevationDeg = -18.0; break;
			case ECineAngle::High:     Geo.ElevationDeg = 30.0; break;
			case ECineAngle::Overhead: Geo.ElevationDeg = 75.0; break;
			default: break;
			}

			// A longer lens needs to sit further back to hold the same framing.
			const double LensScale = Seg.FocalLengthMm > 0.0f ? Seg.FocalLengthMm / 35.0 : 1.0;
			Geo.Distance = FMath::Max(Geo.Radius * FramingFactor(Seg.ShotSize) * LensScale, 60.0);

			if (Seg.ViewSide == ECineViewSide::OverShoulder)
			{
				Geo.Distance *= 0.7;
				Geo.ElevationDeg += 8.0;
			}
		}
		else
		{
			// No subject: anchor the shot on the point the viewport camera is looking at.
			Geo.TargetPoint = ViewLoc + ViewRot.Vector() * 500.0;
			Geo.Radius = 100.0;
			Geo.Distance = 500.0;

			const FVector Offset = ViewLoc - Geo.TargetPoint;
			Geo.AzimuthDeg = FMath::RadiansToDegrees(FMath::Atan2(Offset.Y, Offset.X));
			Geo.ElevationDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Offset.Z / FMath::Max(Offset.Size(), 1.0), -1.0, 1.0)));
		}

		return Geo;
	}

	/**
	 * Geometry for a continuous take: the camera stays where the previous move
	 * left it, and the spherical frame (azimuth/elevation/distance) is derived
	 * from that offset so the next move continues seamlessly. Framing and
	 * view-side words only position the very first move of a take.
	 */
	FShotGeometry ComputeGeometryChained(const FCineShotSegment& Seg, const FVector& PrevPos, const FRotator& PrevRot)
	{
		FShotGeometry Geo;

		if (AActor* Target = Seg.TargetActor.Get())
		{
			const FBox Bounds = Target->GetComponentsBoundingBox(true);
			Geo.bHasTarget = true;
			Geo.TargetPoint = Bounds.IsValid ? Bounds.GetCenter() : Target->GetActorLocation();
			Geo.Radius = Bounds.IsValid ? FMath::Max<double>(Bounds.GetExtent().Size(), 25.0) : 100.0;
		}
		else
		{
			Geo.TargetPoint = PrevPos + PrevRot.Vector() * 500.0;
			Geo.Radius = 100.0;
		}

		const FVector Offset = PrevPos - Geo.TargetPoint;
		Geo.Distance = FMath::Max(Offset.Size(), 1.0);
		Geo.AzimuthDeg = FMath::RadiansToDegrees(FMath::Atan2(Offset.Y, Offset.X));
		Geo.ElevationDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Offset.Z / Geo.Distance, -1.0, 1.0)));
		return Geo;
	}

	/**
	 * Evaluates a segment's camera transform at normalized time.
	 * TMove is the eased move parameter; TReal is wall-clock progress (handheld
	 * noise runs on real time so easing doesn't stretch the shake).
	 */
	struct FShotSampler
	{
		const FCineShotSegment& Seg;
		const FShotGeometry Geo;

		FVector StartPos = FVector::ZeroVector;
		FRotator StartRot = FRotator::ZeroRotator;
		FVector FwdVec = FVector::ForwardVector;
		FVector RightVec = FVector::RightVector;
		bool bAim = false;
		bool bIsOrbit = false;
		bool bIsPanTilt = false;
		/** Continuous-take mode: this segment starts where the previous one ended. */
		bool bChained = false;

		FShotSampler(const FCineShotSegment& InSeg, const FShotGeometry& InGeo, const FRotator& ViewRot, bool bInChained = false)
			: Seg(InSeg)
			, Geo(InGeo)
			, bChained(bInChained)
		{
			bIsOrbit = Seg.Move == ECineMoveType::OrbitCW || Seg.Move == ECineMoveType::OrbitCCW;
			bIsPanTilt =
				Seg.Move == ECineMoveType::PanLeft || Seg.Move == ECineMoveType::PanRight ||
				Seg.Move == ECineMoveType::TiltUp || Seg.Move == ECineMoveType::TiltDown;

			StartPos = Geo.TargetPoint + SphericalOffset(Geo.AzimuthDeg, Geo.ElevationDeg) * Geo.Distance;
			StartRot = Geo.bHasTarget ? (Geo.TargetPoint - StartPos).Rotation() : ViewRot;

			// Orbits always pivot around the target point, even the target-less
			// viewport-anchored kind, or the move would read as a weird strafe.
			bAim = !bIsPanTilt &&
				(bIsOrbit ||
				 (Geo.bHasTarget && Seg.bLookAtTarget) ||
				 (Seg.Move == ECineMoveType::Flyover && Geo.bHasTarget));

			FwdVec = StartRot.Vector();
			RightVec = FRotationMatrix(StartRot).GetUnitAxis(EAxis::Y);
		}

		void Sample(double TMove, double TReal, FVector& OutPos, FRotator& OutRot) const
		{
			const double Amount = Seg.MoveAmount;
			double Az = Geo.AzimuthDeg;
			const double El = Geo.ElevationDeg;
			double Dist = Geo.Distance;
			FVector Pos = StartPos;

			switch (Seg.Move)
			{
			case ECineMoveType::Static:
			case ECineMoveType::ZoomIn:   // zooms move the lens, not the camera
			case ECineMoveType::ZoomOut:
				break;

			case ECineMoveType::DollyIn:
			{
				const double D = Amount > 0.0 ? Amount : Dist * 0.5;
				Dist = FMath::Max(Dist - D * TMove, Geo.Radius * 1.05);
				Pos = Geo.TargetPoint + SphericalOffset(Az, El) * Dist;
				break;
			}
			case ECineMoveType::DollyOut:
			{
				const double D = Amount > 0.0 ? Amount : Dist;
				Pos = Geo.TargetPoint + SphericalOffset(Az, El) * (Dist + D * TMove);
				break;
			}
			case ECineMoveType::OrbitCW:
			case ECineMoveType::OrbitCCW:
			{
				const double Deg = Amount > 0.0 ? Amount : 90.0;
				Az += (Seg.Move == ECineMoveType::OrbitCW ? Deg : -Deg) * TMove;
				Pos = Geo.TargetPoint + SphericalOffset(Az, El) * Dist;
				break;
			}
			case ECineMoveType::TruckLeft:
			case ECineMoveType::TruckRight:
			{
				const double D = Amount > 0.0 ? Amount : FMath::Max(Dist * 0.75, 300.0);
				Pos = StartPos + RightVec * (Seg.Move == ECineMoveType::TruckRight ? D : -D) * TMove;
				break;
			}
			case ECineMoveType::CraneUp:
			case ECineMoveType::CraneDown:
			{
				const double D = Amount > 0.0 ? Amount : 300.0;
				Pos = StartPos + FVector::UpVector * (Seg.Move == ECineMoveType::CraneUp ? D : -D) * TMove;
				break;
			}
			case ECineMoveType::Flyover:
			{
				if (bChained && Geo.bHasTarget)
				{
					// Continue from the current position: fly over the target to the
					// mirrored side, arcing up over the subject on the way.
					const FVector End(
						2.0 * Geo.TargetPoint.X - StartPos.X,
						2.0 * Geo.TargetPoint.Y - StartPos.Y,
						StartPos.Z);
					Pos = FMath::Lerp(StartPos, End, TMove);
					Pos.Z += FMath::Sin(TMove * PI) * FMath::Max(Geo.Radius * 1.5, 300.0);
				}
				else if (Geo.bHasTarget)
				{
					const double Travel = Amount > 0.0 ? Amount : FMath::Max(Dist * 2.0, 3000.0);
					const double Height = FMath::Max(Geo.Radius * 2.5, 500.0);
					const FVector AzDir = SphericalOffset(Az, 0.0);
					const FVector PathStart = Geo.TargetPoint + AzDir * (Travel * 0.5) + FVector(0.0, 0.0, Height);
					const FVector PathEnd = Geo.TargetPoint - AzDir * (Travel * 0.5) + FVector(0.0, 0.0, Height);
					Pos = FMath::Lerp(PathStart, PathEnd, TMove);
				}
				else
				{
					const double Travel = Amount > 0.0 ? Amount : 3000.0;
					Pos = StartPos + FwdVec * Travel * TMove;
				}
				break;
			}
			case ECineMoveType::PanLeft:
			case ECineMoveType::PanRight:
			case ECineMoveType::TiltUp:
			case ECineMoveType::TiltDown:
				break; // rotation-only, handled below
			}

			FRotator Rot = StartRot;
			if (bIsPanTilt)
			{
				const bool bIsPan = Seg.Move == ECineMoveType::PanLeft || Seg.Move == ECineMoveType::PanRight;
				const double Deg = (Amount > 0.0 ? Amount : (bIsPan ? 45.0 : 25.0)) * TMove;
				switch (Seg.Move)
				{
				case ECineMoveType::PanLeft:  Rot.Yaw -= Deg; break;
				case ECineMoveType::PanRight: Rot.Yaw += Deg; break;
				case ECineMoveType::TiltUp:   Rot.Pitch += Deg; break;
				case ECineMoveType::TiltDown: Rot.Pitch -= Deg; break;
				default: break;
				}
			}
			else if (bAim)
			{
				Rot = (Geo.TargetPoint - Pos).Rotation();
			}

			if (Seg.HandheldIntensity > 0.0f)
			{
				const double Time = TReal * Seg.DurationSeconds;
				auto Noise = [Time](int32 Seed)
				{
					return (double)FMath::PerlinNoise1D((float)(Time * 1.3) + Seed * 37.77f);
				};
				Pos += FVector(Noise(1), Noise(2), Noise(3)) * (Seg.HandheldIntensity * 4.0);
				Rot.Pitch += Noise(4) * Seg.HandheldIntensity * 0.9;
				Rot.Yaw += Noise(5) * Seg.HandheldIntensity * 0.9;
				Rot.Roll += Noise(6) * Seg.HandheldIntensity * 0.5;
			}

			Rot.Roll += Seg.DutchAngleDeg;

			OutPos = Pos;
			OutRot = Rot;
		}

		/** Curved paths and noise can't be represented by two keys; everything else can. */
		bool NeedsDenseKeys() const
		{
			if (Seg.HandheldIntensity > 0.0f)
			{
				return true;
			}
			if (bIsOrbit || Seg.Move == ECineMoveType::Flyover)
			{
				return true;
			}
			// A straight-line move that keeps re-aiming at the target bends the rotation curve.
			const bool bLinearMove =
				Seg.Move == ECineMoveType::TruckLeft || Seg.Move == ECineMoveType::TruckRight ||
				Seg.Move == ECineMoveType::CraneUp || Seg.Move == ECineMoveType::CraneDown;
			return bAim && bLinearMove;
		}
	};

	/** Euler state carried across key batches so rotation channels never snap through 0/360. */
	struct FEulerContinuity
	{
		bool bValid = false;
		double Roll = 0.0;
		double Pitch = 0.0;
		double Yaw = 0.0;
	};

	void AddTransformKeys(UMovieScene3DTransformSection* Section, const FShotSampler& Sampler, FFrameNumber SegStart, FFrameRate TickResolution, FEulerContinuity& Euler, bool bSkipFirstKey = false)
	{
		const FCineShotSegment& Seg = Sampler.Seg;

		TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		if (Channels.Num() < 9)
		{
			return;
		}
		Channels[6]->SetDefault(1.0);
		Channels[7]->SetDefault(1.0);
		Channels[8]->SetDefault(1.0);

		const bool bDense = Sampler.NeedsDenseKeys();
		const double Duration = FMath::Max(Seg.DurationSeconds, 0.1);
		const int32 SamplesPerSecond = Seg.HandheldIntensity > 0.0f ? 10 : 6;
		const int32 NumKeys = bDense ? FMath::Clamp((int32)(Duration * SamplesPerSecond), 8, 400) + 1 : 2;
		// Dense samples are close enough together that linear interpolation is exact;
		// two-key shots get cubic keys whose auto tangents give the ease for free.
		const bool bLinearKeys = bDense || Seg.Easing == ECineEasing::Linear;

		for (int32 K = 0; K < NumKeys; ++K)
		{
			if (K == 0 && bSkipFirstKey)
			{
				// Continuous take: the previous segment's end key doubles as this segment's start.
				continue;
			}

			const double T = (double)K / (NumKeys - 1);
			const double TMove = (bDense && Seg.Easing == ECineEasing::EaseInOut) ? T * T * (3.0 - 2.0 * T) : T;

			FVector Pos;
			FRotator Rot;
			Sampler.Sample(TMove, T, Pos, Rot);

			// Keep euler channels continuous so a 360 orbit doesn't snap back through 0.
			if (!Euler.bValid)
			{
				Euler.Roll = Rot.Roll;
				Euler.Pitch = Rot.Pitch;
				Euler.Yaw = Rot.Yaw;
				Euler.bValid = true;
			}
			else
			{
				Euler.Roll += FMath::FindDeltaAngleDegrees(Euler.Roll, Rot.Roll);
				Euler.Pitch += FMath::FindDeltaAngleDegrees(Euler.Pitch, Rot.Pitch);
				Euler.Yaw += FMath::FindDeltaAngleDegrees(Euler.Yaw, Rot.Yaw);
			}

			const FFrameNumber Frame = SegStart + TickResolution.AsFrameTime(T * Seg.DurationSeconds).RoundToFrame();
			const double Values[6] = { Pos.X, Pos.Y, Pos.Z, Euler.Roll, Euler.Pitch, Euler.Yaw };
			for (int32 Ch = 0; Ch < 6; ++Ch)
			{
				if (bLinearKeys)
				{
					Channels[Ch]->AddLinearKey(Frame, Values[Ch]);
				}
				else
				{
					Channels[Ch]->AddCubicKey(Frame, Values[Ch]);
				}
			}
		}
	}
}

FCineSceneContext FShotPlanExecutor::BuildSceneContext()
{
	FCineSceneContext Ctx;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || !Actor->IsListedInSceneOutliner())
			{
				continue;
			}
			// Skip level plumbing and our own output so "the camera" never matches a spawned shot.
			if (Actor->IsA<AWorldSettings>() || Actor->IsA<ABrush>() || Actor->IsA<ACineCameraActor>())
			{
				continue;
			}

			FCineSceneActorInfo Info;
			Info.Actor = Actor;
			Info.Label = Actor->GetActorLabel();
			const FBox Bounds = Actor->GetComponentsBoundingBox(true);
			Info.Location = Bounds.IsValid ? Bounds.GetCenter() : Actor->GetActorLocation();
			Info.BoundsRadius = Bounds.IsValid ? FMath::Max<double>(Bounds.GetExtent().Size(), 25.0) : 100.0;
			Info.Facing = Actor->GetActorRotation();
			Ctx.Actors.Add(MoveTemp(Info));
		}
	}

	if (GCurrentLevelEditingViewportClient)
	{
		Ctx.ViewportLocation = GCurrentLevelEditingViewportClient->GetViewLocation();
		Ctx.ViewportRotation = GCurrentLevelEditingViewportClient->GetViewRotation();
	}

	return Ctx;
}

FCineExecuteResult FShotPlanExecutor::Execute(const FCineShotPlan& Plan)
{
	using namespace CineDirectorExec;

	FCineExecuteResult Result;

	if (Plan.Segments.Num() == 0)
	{
		Result.Error = LOCTEXT("EmptyPlan", "The shot plan contains no shots.");
		return Result;
	}

	ULevelSequence* Sequence = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (!Sequence || !Sequence->GetMovieScene())
	{
		Result.Error = LOCTEXT("NoSequence", "No Level Sequence is open in Sequencer. Open one first (or create one via the Cinematics toolbar button).");
		return Result;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		Result.Error = LOCTEXT("NoWorld", "No editor world available.");
		return Result;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();

	FVector ViewLoc = FVector::ZeroVector;
	FRotator ViewRot = FRotator::ZeroRotator;
	if (GCurrentLevelEditingViewportClient)
	{
		ViewLoc = GCurrentLevelEditingViewportClient->GetViewLocation();
		ViewRot = GCurrentLevelEditingViewportClient->GetViewRotation();
	}

	// Append after whatever camera cuts already exist so re-running extends the edit.
	FFrameNumber Cursor = MovieScene->GetPlaybackRange().GetLowerBoundValue();
	if (UMovieSceneTrack* ExistingCuts = MovieScene->GetCameraCutTrack())
	{
		for (UMovieSceneSection* Section : ExistingCuts->GetAllSections())
		{
			if (Section && Section->HasEndFrame())
			{
				Cursor = FMath::Max(Cursor, Section->GetRange().GetUpperBoundValue());
			}
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateShotsTransaction", "CineDirector: Create Shots"));
	Sequence->Modify();
	MovieScene->Modify();

	UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
	if (Plan.bCreateCameraCuts && !CutTrack)
	{
		CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
	}
	if (CutTrack)
	{
		CutTrack->Modify();
	}

	if (Plan.bOneContinuousShot)
	{
		// ---- One camera, every move chained into a single unbroken take -------
		const FCineShotSegment& First = Plan.Segments[0];
		const FShotGeometry FirstGeo = ComputeGeometry(First, ViewLoc, ViewRot);
		const FShotSampler FirstSampler(First, FirstGeo, ViewRot);

		FVector StartPos;
		FRotator StartRot;
		FirstSampler.Sample(0.0, 0.0, StartPos, StartRot);

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(StartPos, StartRot, SpawnParams);
		if (!Camera)
		{
			Result.Error = LOCTEXT("TakeSpawnFailed", "Failed to spawn the take's camera.");
			return Result;
		}

		const int32 TakeNumber = (CutTrack ? CutTrack->GetAllSections().Num() : 0) + 1;
		const FString Label = First.CameraLabel.IsEmpty()
			? FString::Printf(TEXT("CineDirector Take %d"), TakeNumber)
			: First.CameraLabel;
		Camera->SetActorLabel(Label);

		UCineCameraComponent* Lens = Camera->GetCineCameraComponent();
		float CurrentFocal = First.FocalLengthMm > 0.0f ? First.FocalLengthMm : 35.0f;
		Lens->CurrentFocalLength = CurrentFocal;
		Lens->CurrentAperture = First.Aperture > 0.0f ? First.Aperture : 2.8f;

		const FGuid CamGuid = MovieScene->AddPossessable(Label, Camera->GetClass());
		Sequence->BindPossessableObject(CamGuid, *Camera, World);

		UMovieScene3DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CamGuid);
		UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
		TransformTrack->AddSection(*TransformSection);

		const FFrameNumber TakeStart = Cursor;
		FEulerContinuity Euler;

		float MaxGrain = 0.0f, MaxVignette = 0.0f, MaxFringe = 0.0f, MaxBloom = 0.0f, MaxFlare = 0.0f;
		bool bTrackingFocusSet = false;
		TArray<TPair<FFrameNumber, float>> FocalKeys;
		TArray<TPair<FFrameNumber, float>> FocusKeys;

		FVector PrevPos = StartPos;
		FRotator PrevRot = StartRot;

		int32 MoveIndex = 1;
		for (const FCineShotSegment& Seg : Plan.Segments)
		{
			const bool bFirst = (MoveIndex == 1);
			const FShotGeometry Geo = bFirst ? FirstGeo : ComputeGeometryChained(Seg, PrevPos, PrevRot);
			const FShotSampler Sampler(Seg, Geo, bFirst ? ViewRot : PrevRot, /*bInChained*/ !bFirst);

			FVector SegStartPos, SegEndPos;
			FRotator SegStartRot, SegEndRot;
			Sampler.Sample(0.0, 0.0, SegStartPos, SegStartRot);
			Sampler.Sample(1.0, 1.0, SegEndPos, SegEndRot);

			FFrameNumber DurFrames = TickResolution.AsFrameTime(FMath::Max(Seg.DurationSeconds, 0.1)).RoundToFrame();
			DurFrames = FMath::Max(DurFrames, FFrameNumber(1));
			const FFrameNumber SegStart = Cursor;
			const FFrameNumber SegEnd = SegStart + DurFrames;

			AddTransformKeys(TransformSection, Sampler, SegStart, TickResolution, Euler, /*bSkipFirstKey*/ !bFirst);

			// Zooms and mid-take lens changes become keys on one shared focal-length track.
			if (Seg.Move == ECineMoveType::ZoomIn || Seg.Move == ECineMoveType::ZoomOut)
			{
				float EndFocal = Seg.MoveAmount > 0.0
					? (float)Seg.MoveAmount
					: (Seg.Move == ECineMoveType::ZoomIn ? CurrentFocal * 2.0f : CurrentFocal * 0.5f);
				EndFocal = FMath::Clamp(EndFocal, 4.0f, 1000.0f);
				FocalKeys.Emplace(SegStart, CurrentFocal);
				FocalKeys.Emplace(SegEnd, EndFocal);
				CurrentFocal = EndFocal;
			}
			else if (!bFirst && Seg.FocalLengthMm > 0.0f && !FMath::IsNearlyEqual(Seg.FocalLengthMm, CurrentFocal))
			{
				FocalKeys.Emplace(SegStart, CurrentFocal);
				FocalKeys.Emplace(SegEnd, Seg.FocalLengthMm);
				CurrentFocal = Seg.FocalLengthMm;
			}

			// Focus: rack focus becomes manual-distance keys; otherwise the first
			// tracking request wins for the whole take.
			if (Seg.RackFocusToActor.IsValid() && Seg.TargetActor.IsValid() && !bTrackingFocusSet)
			{
				FocusKeys.Emplace(SegStart, (float)FVector::Dist(SegStartPos, ActorCenter(Seg.TargetActor.Get())));
				FocusKeys.Emplace(SegEnd, (float)FVector::Dist(SegEndPos, ActorCenter(Seg.RackFocusToActor.Get())));
			}
			else if (Seg.bTrackFocus && Seg.TargetActor.IsValid() && !bTrackingFocusSet && FocusKeys.Num() == 0)
			{
				Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
				Lens->FocusSettings.TrackingFocusSettings.ActorToTrack = Seg.TargetActor.Get();
				bTrackingFocusSet = true;
			}

			MaxGrain = FMath::Max(MaxGrain, Seg.FilmGrainIntensity);
			MaxVignette = FMath::Max(MaxVignette, Seg.VignetteIntensity);
			MaxFringe = FMath::Max(MaxFringe, Seg.ChromaticAberrationIntensity);
			MaxBloom = FMath::Max(MaxBloom, Seg.BloomIntensity);
			MaxFlare = FMath::Max(MaxFlare, Seg.LensFlareIntensity);

			FString NoteLine = FString::Printf(TEXT("Move %d:"), MoveIndex);
			if (!Seg.TargetLabel.IsEmpty())
			{
				NoteLine += FString::Printf(TEXT(" on '%s'"), *Seg.TargetLabel);
			}
			if (!Seg.RackFocusToLabel.IsEmpty())
			{
				NoteLine += FString::Printf(TEXT(", rack focus to '%s'"), *Seg.RackFocusToLabel);
			}
			NoteLine += FString::Printf(TEXT(" — %.1fs"), Seg.DurationSeconds);
			Result.Notes.Add(MoveTemp(NoteLine));
			for (const FString& Note : Seg.ParseNotes)
			{
				Result.Notes.Add(FString::Printf(TEXT("Move %d: %s"), MoveIndex, *Note));
			}

			UE_LOG(LogCineDirector, Log, TEXT("Take move %d: clause \"%s\", target '%s', %.1fs, frames %d-%d"),
				MoveIndex, *Seg.RawText,
				Seg.TargetLabel.IsEmpty() ? TEXT("<none>") : *Seg.TargetLabel,
				Seg.DurationSeconds, SegStart.Value, SegEnd.Value);

			Result.TotalDurationSeconds += Seg.DurationSeconds;
			PrevPos = SegEndPos;
			PrevRot = SegEndRot;
			Cursor = SegEnd;
			++MoveIndex;
		}

		TransformSection->SetRange(TRange<FFrameNumber>(TakeStart, Cursor));

		if (FocusKeys.Num() > 0 && !bTrackingFocusSet)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
		}
		else if (!bTrackingFocusSet && FirstGeo.bHasTarget)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			Lens->FocusSettings.ManualFocusDistance = FVector::Dist(StartPos, FirstGeo.TargetPoint);
		}

		FPostProcessSettings& PP = Lens->PostProcessSettings;
		if (MaxGrain > 0.0f) { PP.bOverride_FilmGrainIntensity = true; PP.FilmGrainIntensity = MaxGrain; }
		if (MaxVignette > 0.0f) { PP.bOverride_VignetteIntensity = true; PP.VignetteIntensity = MaxVignette; }
		if (MaxFringe > 0.0f) { PP.bOverride_SceneFringeIntensity = true; PP.SceneFringeIntensity = MaxFringe; }
		if (MaxBloom > 0.0f) { PP.bOverride_BloomIntensity = true; PP.BloomIntensity = MaxBloom; }
		if (MaxFlare > 0.0f) { PP.bOverride_LensFlareIntensity = true; PP.LensFlareIntensity = MaxFlare; }

		FGuid LensGuid;
		auto GetLensBinding = [&]() -> FGuid
		{
			if (!LensGuid.IsValid())
			{
				LensGuid = MovieScene->AddPossessable(Lens->GetName(), Lens->GetClass());
				if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(LensGuid))
				{
					Possessable->SetParent(CamGuid, MovieScene);
				}
				Sequence->BindPossessableObject(LensGuid, *Lens, Camera);
			}
			return LensGuid;
		};

		auto AddLensKeysTrack = [&](FName PropertyName, const FString& PropertyPath, const TArray<TPair<FFrameNumber, float>>& Keys)
		{
			UMovieSceneFloatTrack* Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(GetLensBinding());
			Track->SetPropertyNameAndPath(PropertyName, PropertyPath);
			UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
			Section->SetRange(TRange<FFrameNumber>(TakeStart, Cursor));
			Track->AddSection(*Section);
			TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			if (Channels.Num() > 0)
			{
				for (const TPair<FFrameNumber, float>& Key : Keys)
				{
					Channels[0]->AddCubicKey(Key.Key, Key.Value);
				}
			}
		};

		if (FocalKeys.Num() > 0)
		{
			AddLensKeysTrack(TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"), FocalKeys);
		}
		if (FocusKeys.Num() > 0 && !bTrackingFocusSet)
		{
			AddLensKeysTrack(TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"), FocusKeys);
		}

		if (Plan.bCreateCameraCuts && CutTrack)
		{
			if (UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(CamGuid), TakeStart))
			{
				CutSection->SetRange(TRange<FFrameNumber>(TakeStart, Cursor));
			}
		}

		Result.Notes.Insert(FString::Printf(TEXT("Continuous take: camera '%s', %d moves"), *Label, Plan.Segments.Num()), 0);
		Result.NumShots = 1;

		const TRange<FFrameNumber> TakePlayback = MovieScene->GetPlaybackRange();
		if (Cursor > TakePlayback.GetUpperBoundValue())
		{
			MovieScene->SetPlaybackRange(TRange<FFrameNumber>(TakePlayback.GetLowerBoundValue(), Cursor));
		}

		MovieScene->MarkPackageDirty();
		ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();

		Result.bSuccess = true;
		UE_LOG(LogCineDirector, Log, TEXT("Created continuous take '%s': %d moves, %.1fs total."),
			*Label, Plan.Segments.Num(), Result.TotalDurationSeconds);
		return Result;
	}

	int32 ShotIndex = 1;
	for (const FCineShotSegment& Seg : Plan.Segments)
	{
		const FShotGeometry Geo = ComputeGeometry(Seg, ViewLoc, ViewRot);
		const FShotSampler Sampler(Seg, Geo, ViewRot);

		FVector StartPos, EndPos;
		FRotator StartRot, EndRot;
		Sampler.Sample(0.0, 0.0, StartPos, StartRot);
		Sampler.Sample(1.0, 1.0, EndPos, EndRot);

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(StartPos, StartRot, SpawnParams);
		if (!Camera)
		{
			Result.Notes.Add(FString::Printf(TEXT("Shot %d: failed to spawn a camera — skipped."), ShotIndex));
			++ShotIndex;
			continue;
		}

		const FString Label = Seg.CameraLabel.IsEmpty()
			? FString::Printf(TEXT("CineDirector Shot %d"), ShotIndex)
			: Seg.CameraLabel;
		Camera->SetActorLabel(Label);

		UCineCameraComponent* Lens = Camera->GetCineCameraComponent();
		Lens->CurrentFocalLength = Seg.FocalLengthMm > 0.0f ? Seg.FocalLengthMm : 35.0f;
		Lens->CurrentAperture = Seg.Aperture > 0.0f ? Seg.Aperture : 2.8f;

		FPostProcessSettings& PP = Lens->PostProcessSettings;
		if (Seg.FilmGrainIntensity > 0.0f)
		{
			PP.bOverride_FilmGrainIntensity = true;
			PP.FilmGrainIntensity = Seg.FilmGrainIntensity;
		}
		if (Seg.VignetteIntensity > 0.0f)
		{
			PP.bOverride_VignetteIntensity = true;
			PP.VignetteIntensity = Seg.VignetteIntensity;
		}
		if (Seg.ChromaticAberrationIntensity > 0.0f)
		{
			PP.bOverride_SceneFringeIntensity = true;
			PP.SceneFringeIntensity = Seg.ChromaticAberrationIntensity;
		}
		if (Seg.BloomIntensity > 0.0f)
		{
			PP.bOverride_BloomIntensity = true;
			PP.BloomIntensity = Seg.BloomIntensity;
		}
		if (Seg.LensFlareIntensity > 0.0f)
		{
			PP.bOverride_LensFlareIntensity = true;
			PP.LensFlareIntensity = Seg.LensFlareIntensity;
		}

		const bool bRackFocus = Seg.RackFocusToActor.IsValid() && Seg.TargetActor.IsValid();
		if (bRackFocus)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
		}
		else if (Seg.bTrackFocus && Seg.TargetActor.IsValid())
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Tracking;
			Lens->FocusSettings.TrackingFocusSettings.ActorToTrack = Seg.TargetActor.Get();
		}
		else if (Geo.bHasTarget)
		{
			Lens->FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
			Lens->FocusSettings.ManualFocusDistance = FVector::Dist(StartPos, Geo.TargetPoint);
		}

		const FGuid CamGuid = MovieScene->AddPossessable(Label, Camera->GetClass());
		Sequence->BindPossessableObject(CamGuid, *Camera, World);

		FFrameNumber DurFrames = TickResolution.AsFrameTime(FMath::Max(Seg.DurationSeconds, 0.1)).RoundToFrame();
		DurFrames = FMath::Max(DurFrames, FFrameNumber(1));
		const FFrameNumber SegStart = Cursor;
		const FFrameNumber SegEnd = SegStart + DurFrames;

		UMovieScene3DTransformTrack* TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CamGuid);
		UMovieScene3DTransformSection* TransformSection = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
		TransformSection->SetRange(TRange<FFrameNumber>(SegStart, SegEnd));
		TransformTrack->AddSection(*TransformSection);
		FEulerContinuity Euler;
		AddTransformKeys(TransformSection, Sampler, SegStart, TickResolution, Euler);

		// The lens component only needs its own binding when a lens property is animated.
		FGuid LensGuid;
		auto GetLensBinding = [&]() -> FGuid
		{
			if (!LensGuid.IsValid())
			{
				LensGuid = MovieScene->AddPossessable(Lens->GetName(), Lens->GetClass());
				if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(LensGuid))
				{
					Possessable->SetParent(CamGuid, MovieScene);
				}
				Sequence->BindPossessableObject(LensGuid, *Lens, Camera);
			}
			return LensGuid;
		};

		auto AddLensFloatTrack = [&](FName PropertyName, const FString& PropertyPath, float StartValue, float EndValue)
		{
			UMovieSceneFloatTrack* Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(GetLensBinding());
			Track->SetPropertyNameAndPath(PropertyName, PropertyPath);
			UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
			Section->SetRange(TRange<FFrameNumber>(SegStart, SegEnd));
			Track->AddSection(*Section);

			TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
			if (Channels.Num() > 0)
			{
				if (Seg.Easing == ECineEasing::Linear)
				{
					Channels[0]->AddLinearKey(SegStart, StartValue);
					Channels[0]->AddLinearKey(SegEnd, EndValue);
				}
				else
				{
					Channels[0]->AddCubicKey(SegStart, StartValue);
					Channels[0]->AddCubicKey(SegEnd, EndValue);
				}
			}
		};

		if (Seg.Move == ECineMoveType::ZoomIn || Seg.Move == ECineMoveType::ZoomOut)
		{
			const float StartFocal = Lens->CurrentFocalLength;
			float EndFocal = Seg.MoveAmount > 0.0
				? (float)Seg.MoveAmount
				: (Seg.Move == ECineMoveType::ZoomIn ? StartFocal * 2.0f : StartFocal * 0.5f);
			EndFocal = FMath::Clamp(EndFocal, 4.0f, 1000.0f);
			AddLensFloatTrack(TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"), StartFocal, EndFocal);
		}

		if (bRackFocus)
		{
			const FVector FromPoint = ActorCenter(Seg.TargetActor.Get());
			const FVector ToPoint = ActorCenter(Seg.RackFocusToActor.Get());
			AddLensFloatTrack(TEXT("ManualFocusDistance"), TEXT("FocusSettings.ManualFocusDistance"),
				(float)FVector::Dist(StartPos, FromPoint), (float)FVector::Dist(EndPos, ToPoint));
		}

		if (Plan.bCreateCameraCuts && CutTrack)
		{
			if (UMovieSceneCameraCutSection* CutSection = CutTrack->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(CamGuid), SegStart))
			{
				CutSection->SetRange(TRange<FFrameNumber>(SegStart, SegEnd));
			}
		}

		FString NoteLine = FString::Printf(TEXT("Shot %d: '%s'"), ShotIndex, *Label);
		if (!Seg.TargetLabel.IsEmpty())
		{
			NoteLine += FString::Printf(TEXT(" on '%s'"), *Seg.TargetLabel);
		}
		if (!Seg.RackFocusToLabel.IsEmpty())
		{
			NoteLine += FString::Printf(TEXT(", rack focus to '%s'"), *Seg.RackFocusToLabel);
		}
		NoteLine += FString::Printf(TEXT(" — %.1fs"), Seg.DurationSeconds);
		Result.Notes.Add(MoveTemp(NoteLine));
		for (const FString& Note : Seg.ParseNotes)
		{
			Result.Notes.Add(FString::Printf(TEXT("Shot %d: %s"), ShotIndex, *Note));
		}

		UE_LOG(LogCineDirector, Log, TEXT("Shot %d: camera '%s', target '%s', clause \"%s\", %.1fs, frames %d-%d"),
			ShotIndex, *Label,
			Seg.TargetLabel.IsEmpty() ? TEXT("<none>") : *Seg.TargetLabel,
			*Seg.RawText, Seg.DurationSeconds, SegStart.Value, SegEnd.Value);
		for (const FString& Note : Seg.ParseNotes)
		{
			UE_LOG(LogCineDirector, Warning, TEXT("Shot %d: %s"), ShotIndex, *Note);
		}

		Result.TotalDurationSeconds += Seg.DurationSeconds;
		++Result.NumShots;
		Cursor = SegEnd;
		++ShotIndex;
	}

	if (Result.NumShots == 0)
	{
		Result.Error = LOCTEXT("NothingCreated", "Couldn't create any cameras from the plan.");
		return Result;
	}

	const TRange<FFrameNumber> Playback = MovieScene->GetPlaybackRange();
	if (Cursor > Playback.GetUpperBoundValue())
	{
		MovieScene->SetPlaybackRange(TRange<FFrameNumber>(Playback.GetLowerBoundValue(), Cursor));
	}

	MovieScene->MarkPackageDirty();
	ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();

	Result.bSuccess = true;
	UE_LOG(LogCineDirector, Log, TEXT("Created %d shot(s), %.1fs total, in '%s'."),
		Result.NumShots, Result.TotalDurationSeconds, *Sequence->GetName());
	return Result;
}

#undef LOCTEXT_NAMESPACE
