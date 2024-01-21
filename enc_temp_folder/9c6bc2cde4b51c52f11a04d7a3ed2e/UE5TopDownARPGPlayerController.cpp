// Copyright Epic Games, Inc. All Rights Reserved.

#include "UE5TopDownARPGPlayerController.h"
#include "GameFramework/Pawn.h"
#include "Blueprint/AIBlueprintHelperLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "UE5TopDownARPGCharacter.h"
#include "UE5TopDownARPGHUD.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "UE5TopDownARPG.h"
#include "NavigationSystem.h"
#include "Engine/TextRenderActor.h"
#include "Components/TextRenderComponent.h"
#include "Internationalization/Text.h"
#include "Kismet/GameplayStatics.h"
#include <map>
#include <queue>
#include <climits>

const float SQRT_OF_2 = FMath::Sqrt(2.f);
const float CELL_SIZE = 100.f;
const float H_CELL_SIZE = CELL_SIZE / 2;
const float Q_CELL_SIZE = CELL_SIZE / 4;
const float CELL_DIAG = CELL_SIZE * SQRT_OF_2;
const float H_CELL_DIAG = CELL_SIZE * SQRT_OF_2;

const FVector COST_TRACE_Y_START = { 0.f, 0.f, 250.f };
const FVector COST_TRACE_DIRECTION {0.f, 0.f, -100};
const FIntVector2 GRIDSIZE{ 30, 30 };

namespace Dirs
{
	enum EDirection : uint8_t {
		SouthWest = 0,
		West,
		NorthWest,
		North,
		NorthEast,
		East,
		SouthEast,
		South
	};

	static const std::map<EDirection, FIntVector2> DIRS = { //TODO OPTIMIZE FLAGS
	{ EDirection::SouthWest,  {-1, -1} },
	{ EDirection::West,       {-1, 0 } },
	{ EDirection::NorthWest,  {-1, 1 } },
	{ EDirection::North,      { 0, 1 } },
	{ EDirection::NorthEast,  { 1, 1 } },
	{ EDirection::East,       { 1, 0 } },
	{ EDirection::SouthEast,  { 1, -1} },
	{ EDirection::South,      { 0, -1} }
	};

	static const FIntVector2 SW{ -1, -1 };
	static const FIntVector2 W { -1,  0 };
	static const FIntVector2 NW{ -1,  1 };
	static const FIntVector2 N {  0,  1 };
	static const FIntVector2 NE{  1,  1 };
	static const FIntVector2 E {  1,  0 };
	static const FIntVector2 SE{  1, -1 };
	static const FIntVector2 S {  0, -1 };

	static const bool IsDiagonal(EDirection Dir) { return Dir % 2 == 0; }
	static const FIntVector2 Next(EDirection Dir) { return DIRS.at( EDirection((Dir + 1) % 8) ); }
	static const FIntVector2 Prev(EDirection Dir) { return DIRS.at( EDirection((Dir + 7) % 8) ); }
}

static const int ApplyDir(const int Idx, const FIntVector2& Dir) {
	int Result = Idx + Dir.Y * GRIDSIZE.X + Dir.X;
	return Result;
}

struct FlowField {
	Dirs::EDirection Dir : 4;
	uint8_t Completed : 1; // TODO Change this to Pathable in next version
	uint8_t LOS : 1;       // NOT USED YET
};

struct IntegrationField {
	float IntegratedCost = FLT_MAX;
	uint8_t WaveFrontBlocked : 1;
	uint8_t LOS : 1;
};

namespace Debug
{
	UWorld* pWorld;
	void Draw(UWorld* World, const TArray<uint8_t>& CostFields)
	{
		ensure(World);
		for (int y = 0; y < GRIDSIZE.Y; ++y)
		{
			for (int x = 0; x < GRIDSIZE.X; ++x)
			{
				FVector RayStart = COST_TRACE_Y_START + FVector(x * CELL_SIZE + CELL_SIZE / 2, y * CELL_SIZE + CELL_SIZE / 2, 0.f);
				FVector RayEnd = RayStart + COST_TRACE_DIRECTION;
				// UE_LOG(LogUE5TopDownARPG, Log, TEXT("CostFields [%d][%d] = [%d]"),i, j, CostFields[i * GRIDSIZE.X + j]);
				DrawDebugDirectionalArrow(World, RayStart, RayEnd, 3.0f, CostFields[y * GRIDSIZE.X + x] == UINT8_MAX ? FColor::Red : FColor::Green, true, -1.f, 0, 2.f);
			}
		}
	}

	void Draw(UWorld* World, const TArray<IntegrationField>& IntegrationFields)
	{
		ensure(World);
		FlushPersistentDebugLines(World);

		for (int y = 0; y < GRIDSIZE.Y; ++y)
		{
			for (int x = 0; x < GRIDSIZE.X; ++x)
			{
				FVector TextStart = { x * CELL_SIZE + CELL_SIZE/2.f, y * CELL_SIZE + CELL_SIZE/2.f, 60.f };
				auto IntegratedCost = FString::FromInt(IntegrationFields[y * GRIDSIZE.X + x].IntegratedCost);
				if (IntegrationFields[y * GRIDSIZE.X + x].IntegratedCost == FLT_MAX)
				{
					IntegratedCost = "MAX";
				}

				// UE_LOG(LogUE5TopDownARPG, Log, TEXT("IntegrationFields [%d][%d] = [%s]"), i, j, *IntegratedCost);

				FActorSpawnParameters SpawnParameters;
				SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
				auto TextActor = World->SpawnActor<ATextRenderActor>(ATextRenderActor::StaticClass(), TextStart, FRotator(90,0,180), SpawnParameters);
				TextActor->GetTextRender()->SetText(FText::FromString(IntegratedCost));
			}
		}
	}

	void Draw(UWorld* World, const TArray<FlowField>& FlowFields)
	{
		ensure(World);
		FlushPersistentDebugLines(World);
		FVector Ray{ 0.f, 0.f, 50.f };

		for (int y = 0; y < GRIDSIZE.Y; ++y)
		{
			for (int x = 0; x < GRIDSIZE.X; ++x)
			{
				if (FlowFields[y * GRIDSIZE.X + x].Completed == false)
				{
					FMatrix Transformation = FMatrix::Identity;
					Transformation.SetOrigin({ x * 100.f + 50 , y * 100.f + 50 , 50.f });
					FMatrix Rotation = FRotationMatrix({ 90.f, 0.f, 180.f });

					FMatrix DonutMatrix = Rotation * Transformation;
					DrawDebug2DDonut(World, DonutMatrix, 5.f, 10.f, 16, FColor::Blue, true);
				}
				else
				{
					Dirs::EDirection Dir = FlowFields[y * GRIDSIZE.X + x].Dir;
					FIntVector2 IntOffset = Dirs::DIRS.at(Dir) * 25;
					FVector Offset{ IntOffset.X * 1.f, IntOffset.Y * 1.f, 0.f };
					Ray.Y = y * 100.f + 50;
					Ray.X = x * 100.f + 50;

					FVector Start = Ray - Offset;
					FVector End = Ray + Offset;

					DrawDebugDirectionalArrow(World, Start, End, 3.f, FColor::Green, true, 9999999.f, 0, 2.f);

					UE_LOG(LogUE5TopDownARPG, Log, TEXT("FlowFields[%d][%d].Dir = [%d]"), y, x, (int)FlowFields[y * GRIDSIZE.X + x].Dir);
					UE_LOG(LogUE5TopDownARPG, Log, TEXT("Start = %s"), *Start.ToString());
					UE_LOG(LogUE5TopDownARPG, Log, TEXT("End = %s"), *End.ToString());
				}
			}
		}
	}

	FVector addZ(FVector2D Vect, float Z)
	{
		return FVector(Vect.X, Vect.Y, Z);
	}
	/*
	Origin in World Coordinates
	*/
	void DrawLOS(FVector2D NormDirection, FVector2D Origin = {0.f,0.f}, int Scale = 1000000)
	{
		FVector2D Direction = NormDirection * Scale;
		FVector2D End = Direction + Origin;

		FVector Origin3D = addZ(Origin, 60.f);
		FVector End3D = addZ(End, 60.f);
		DrawDebugDirectionalArrow(pWorld, Origin3D, End3D, 3.f, FColor::Emerald, true, 9999999.f, 0, 2.f);
		UE_LOG(LogUE5TopDownARPG, Log, TEXT("Draw LOS: Origin3D = %s; End3D = %s"), *Origin3D.ToString(), *End3D.ToString());
	}

	void DrawBox(FIntVector2 At)
	{
		ensure(pWorld);
		FVector Center{
			At.X * CELL_SIZE + H_CELL_SIZE,
			At.Y * CELL_SIZE + H_CELL_SIZE,
			60.f
		};
		FVector Extent = FVector(Q_CELL_SIZE, Q_CELL_SIZE, Q_CELL_SIZE);
		DrawDebugBox(pWorld, Center, Extent, FColor::Orange, true);
		UE_LOG(LogUE5TopDownARPG, Log, TEXT("Draw Box: Center = %s; Extent = %s"), *Center.ToString(), *Extent.ToString());
	}

	void DrawCoords()
	{
		ensure(pWorld);

		for (int y = 0; y < GRIDSIZE.Y; ++y)
		{
			for (int x = 0; x < GRIDSIZE.X; ++x)
			{
				FVector TextStart = { x * CELL_SIZE + H_CELL_SIZE, y * CELL_SIZE, 100.f }; // TODO why x,y reversed?
					
				// UE_LOG(LogUE5TopDownARPG, Log, TEXT("IntegrationFields [%d][%d] = [%s]"), i, j, *IntegratedCost
				FText Coords = FText::FromString(" [" + FString::FromInt(x) + ", " + FString::FromInt(y) + "]");
				FActorSpawnParameters SpawnParameters;
				SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
				auto TextActor = pWorld->SpawnActor<ATextRenderActor>(ATextRenderActor::StaticClass(), TextStart, FRotator(90, 0, 180), SpawnParameters);
				TextActor->GetTextRender()->SetText(Coords);
			}
		}
	}

}

namespace Eikonal {
	const float SIGNIFICANT_COST_REDUCTION = 0.75f; // 25% reduction

	bool IsInGrid(const TArray<uint8_t>& CostFields, const FIntVector2& Cell) { // TODO can we reuse?
		return Cell.X >= 0 && Cell.X < GRIDSIZE.X && Cell.Y >= 0 && Cell.Y < GRIDSIZE.Y;
	}

	bool IsWall(const TArray<uint8_t>& CostFields, const FIntVector2& Cell)
	{
		return CostFields[Cell.Y * GRIDSIZE.X + Cell.X] == UINT8_MAX;
	}

	using VisitCellFunc = const std::function<void(const TArray<uint8_t>& CostFields, TArray<IntegrationField>& IntegrationFields, std::queue<FIntVector2>& WaveFront, const FIntVector2& Cell, float CurrentCost, const FIntVector2& Goal)>&;

	void VisitCell_Finish(const TArray<uint8_t>& CostFields, TArray<IntegrationField>& IntegrationFields, std::queue<FIntVector2>& WaveFront, const FIntVector2& Cell, float CurrentCost, const FIntVector2& Goal) {
		if (IsInGrid(CostFields, Cell) == false || IsWall(CostFields, Cell)) { return; }

		float NewCost = CurrentCost + CostFields[Cell.Y * GRIDSIZE.X + Cell.X];
		float OldCost = IntegrationFields[Cell.Y * GRIDSIZE.X + Cell.X].IntegratedCost;

		if (NewCost < OldCost * SIGNIFICANT_COST_REDUCTION) {
			IntegrationFields[Cell.Y * GRIDSIZE.X + Cell.X].IntegratedCost = NewCost;
			WaveFront.push(Cell);
		}
	}

	FVector2D ToVector2D(const FIntVector2& IntVector2)
	{
		FVector2D Result;
		Result.X = static_cast<float>(IntVector2.X);
		Result.Y = static_cast<float>(IntVector2.Y);
		return Result;
	};

	/*
	Cell & Goal: Grid Coordinates
	*/
	void CalcLOSBorder(const TArray<uint8_t>& CostFields, TArray<IntegrationField>& IntegrationFields, const FIntVector2& Cell, const FIntVector2& Goal)
	{
		FIntVector2 Side;
		FVector2D GoalToCell = ToVector2D(Cell - Goal);
		if (GoalToCell.X * GoalToCell.Y > 0) // Wall is in 1st or 3rd quadrant with Goal as origin
		{
			Side = Cell + Dirs::NW;
			if (IsInGrid(CostFields, Side)
				&& IsWall(CostFields, Side) == false
				&& IsWall(CostFields, Cell + Dirs::W) == false
				&& IsWall(CostFields, Cell + Dirs::N) == false
				)
			{
				UE_LOG(LogUE5TopDownARPG, Log, TEXT("Found LOS Border at: [%d, %d]"), Side.X, Side.Y);

				FVector2D WorldGoal = ToVector2D(Goal * CELL_SIZE) + H_CELL_DIAG; // Center of Cell in World coordinates
				FVector2D WorldCell = ToVector2D(Cell * CELL_SIZE) + H_CELL_DIAG; 
				FVector2D WorldSide = ToVector2D(Side * CELL_SIZE) + H_CELL_DIAG;
				FVector2D WorldMid = (WorldCell + WorldSide) / 2.f;
				FVector2D NormDir = WorldMid - WorldGoal;

				Debug::DrawLOS(NormDir, WorldMid);
				//BresenhamsRay2D(CostFields, NormDir, Midpoint, IntegrationFields);
			}

			Side = Cell + Dirs::SE;
			if (IsInGrid(CostFields, Side)
				&& IsWall(CostFields, Side) == false
				&& IsWall(CostFields, Cell + Dirs::S) == false
				&& IsWall(CostFields, Cell + Dirs::E) == false
				)
			{
				UE_LOG(LogUE5TopDownARPG, Log, TEXT("Found LOS Border at: [%d, %d]"), Side.X, Side.Y);

				FVector2D WorldGoal = ToVector2D(Goal * CELL_SIZE) + H_CELL_DIAG; // Center of Cell in World coordinates
				FVector2D WorldCell = ToVector2D(Cell * CELL_SIZE) + H_CELL_DIAG;
				FVector2D WorldSide = ToVector2D(Side * CELL_SIZE) + H_CELL_DIAG;
				FVector2D WorldMid = (WorldCell + WorldSide) / 2.f;
				FVector2D NormDir = WorldMid - WorldGoal;

				Debug::DrawLOS(NormDir, WorldMid);
				//BresenhamsRay2D(CostFields, NormDir, Midpoint, IntegrationFields);
			}
		}
		else // Wall is in 2nd or 4th quadrant with Goal as origin
		{
			Side = Cell + Dirs::NE;
			if (IsInGrid(CostFields, Side)
				&& IsWall(CostFields, Side) == false
				&& IsWall(CostFields, Cell + Dirs::N) == false
				&& IsWall(CostFields, Cell + Dirs::E) == false
				)
			{
				UE_LOG(LogUE5TopDownARPG, Log, TEXT("Found LOS Border at: [%d, %d]"), Side.X, Side.Y);

				FVector2D WorldGoal = ToVector2D(Goal * CELL_SIZE) + H_CELL_DIAG; // Center of Cell in World coordinates
				FVector2D WorldCell = ToVector2D(Cell * CELL_SIZE) + H_CELL_DIAG;
				FVector2D WorldSide = ToVector2D(Side * CELL_SIZE) + H_CELL_DIAG;
				FVector2D WorldMid = (WorldCell + WorldSide) / 2.f;
				FVector2D NormDir = WorldMid - WorldGoal;

				Debug::DrawLOS(NormDir, WorldMid);
				//BresenhamsRay2D(CostFields, NormDir, Midpoint, IntegrationFields);
			}

			Side = Cell + Dirs::SW;
			if (IsInGrid(CostFields, Side)
				&& IsWall(CostFields, Side) == false
				&& IsWall(CostFields, Cell + Dirs::S) == false
				&& IsWall(CostFields, Cell + Dirs::W) == false
				)
			{
				UE_LOG(LogUE5TopDownARPG, Log, TEXT("Found LOS Border at: [%d, %d]"), Side.X, Side.Y);

				FVector2D WorldGoal = ToVector2D(Goal * CELL_SIZE) + H_CELL_DIAG; // Center of Cell in World coordinates
				FVector2D WorldCell = ToVector2D(Cell * CELL_SIZE) + H_CELL_DIAG;
				FVector2D WorldSide = ToVector2D(Side * CELL_SIZE) + H_CELL_DIAG;
				FVector2D WorldMid = (WorldCell + WorldSide) / 2.f;
				FVector2D NormDir = WorldMid - WorldGoal;

				Debug::DrawLOS(NormDir, WorldMid);
				//BresenhamsRay2D(CostFields, NormDir, Midpoint, IntegrationFields);
			}
		}
	}

	void VisitCell_LOS(const TArray<uint8_t>& CostFields, TArray<IntegrationField>& IntegrationFields, std::queue<FIntVector2>& WaveFront, const FIntVector2& Cell, float CurrentCost, const FIntVector2& Goal) {
		if (IsInGrid(CostFields, Cell) == false || IntegrationFields[Cell.Y * GRIDSIZE.X + Cell.X].WaveFrontBlocked) { return; }

		if (IsWall(CostFields, Cell))
		{
			UE_LOG(LogUE5TopDownARPG, Log, TEXT("Found Wall at: [%d, %d]"), Cell.X, Cell.Y);
			CalcLOSBorder(CostFields, IntegrationFields, Cell, Goal);
		}
		float NewCost = CurrentCost + CostFields[Cell.Y * GRIDSIZE.X + Cell.X];
		float OldCost = IntegrationFields[Cell.Y * GRIDSIZE.X + Cell.X].IntegratedCost;

		if (NewCost < OldCost * SIGNIFICANT_COST_REDUCTION) {  // TODO avoid floats?
			IntegrationFields[Cell.Y * GRIDSIZE.X + Cell.X].IntegratedCost = NewCost;
			WaveFront.push(Cell);
		}
	}

	void PropagateWave(const TArray<uint8_t>& CostFields, TArray<IntegrationField>& IntegrationFields, std::queue<FIntVector2>& WaveFront, VisitCellFunc VisitCell, const FIntVector2& Goal) {

		while (!WaveFront.empty()) {
			FIntVector2 Current = WaveFront.front();
			WaveFront.pop();

			float CurrentCost = IntegrationFields[Current.Y * GRIDSIZE.X + Current.X].IntegratedCost;

			VisitCell(CostFields, IntegrationFields, WaveFront, FIntVector2(Current.X + 1, Current.Y), CurrentCost, Goal);
			VisitCell(CostFields, IntegrationFields, WaveFront, FIntVector2(Current.X - 1, Current.Y), CurrentCost, Goal);
			VisitCell(CostFields, IntegrationFields, WaveFront, FIntVector2(Current.X, Current.Y + 1), CurrentCost, Goal);
			VisitCell(CostFields, IntegrationFields, WaveFront, FIntVector2(Current.X, Current.Y - 1), CurrentCost, Goal);
		}
	}
}

/*
Direction is normalized
*/
void BresenhamsRay2D(const TArray<uint8_t>& CostFields, const FVector2D& Direction, const FIntVector2& Pos, TArray<IntegrationField>& IntegrationFields)
{   // TODO Test
	int x = Pos.X;
	int y = Pos.Y;
	int endX = std::floor(Pos.X + Direction.X * GRIDSIZE.X);
	int endY = std::floor(Pos.Y + Direction.Y * GRIDSIZE.Y);

	int dx = std::abs(endX - x);
	int dy = -std::abs(endY - y);
	int sx = x < endX ? 1 : -1;
	int sy = y < endY ? 1 : -1;
	int err = dx + dy, e2;

	while (true) {
		if (x >= 0 && x < GRIDSIZE.X && y >= 0 && y < GRIDSIZE.Y) {
			if (CostFields[y * GRIDSIZE.X + x] == 255) {
				IntegrationFields[y * GRIDSIZE.X + x].LOS = false;
				break;
			}
			else {
				IntegrationFields[y * GRIDSIZE.X + x].LOS = true;
			}
		}
		else {
			// Break if we are out of bounds
			break;
		}

		if (x == endX && y == endY) break;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x += sx; }
		if (e2 <= dx) { err += dx; y += sy; }
	}
}

void LineOfSightPass(int MapHeight, int MapWidth, const FIntVector2& Goal, OUT TArray<IntegrationField>& IntegrationFields)
{
	// TODO Implement
}

bool IsValidIdx(int Idx)
{
	return Idx >= 0 && Idx < GRIDSIZE.X * GRIDSIZE.Y;
}

void CalculateFlowFields(TArray<IntegrationField>& IntegrationFields, OUT std::queue<int>& Sources, OUT TArray<FlowField>& FlowFields)
{

	auto DiagonalReachable = [&IntegrationFields](const int CurIdx, const Dirs::EDirection NewDir) {
		return IntegrationFields[ApplyDir(CurIdx, Next(NewDir))].IntegratedCost != FLT_MAX && IntegrationFields[ApplyDir(CurIdx, Prev(NewDir))].IntegratedCost != FLT_MAX; // TODO change to binary comparison?
	};

	while (Sources.empty() == false)
	{
		const int CurIdx = Sources.front();
		Sources.pop();
		check(IsValidIdx(CurIdx)); // TODO optimize: executed twice for each Idx

		if (FlowFields[CurIdx].Completed || IntegrationFields[CurIdx].IntegratedCost == 0) // Reached other solution or goal
		{
			continue;
		}

		float BestCost = IntegrationFields[CurIdx].IntegratedCost;
		Dirs::EDirection BestDir;
		int BestIdx = ApplyDir(CurIdx, Dirs::DIRS.at(BestDir));

		for (const auto& pair : Dirs::DIRS) {
			Dirs::EDirection NewDir = pair.first;
			int NewIdx = ApplyDir(CurIdx, pair.second);

			if (IsValidIdx(NewIdx) && IntegrationFields[NewIdx].IntegratedCost != FLT_MAX) { // TODO change to binary comparison?

				if (Dirs::IsDiagonal(NewDir) && DiagonalReachable(CurIdx, NewDir) == false) continue;

				if (BestCost > IntegrationFields[NewIdx].IntegratedCost)
				{
					BestIdx = NewIdx;
					BestCost = IntegrationFields[NewIdx].IntegratedCost;
					BestDir = NewDir;

				}
			}
		}
		FlowFields[CurIdx].Dir = BestDir;
		FlowFields[CurIdx].Completed = true;
		Sources.push(BestIdx);
		
		UE_LOG(LogUE5TopDownARPG, Log, TEXT("CalculateFlowFields [%d].Dir = %d"), CurIdx, FlowFields[CurIdx].Dir);
	}
}

bool CalculateCostFields(UWorld* World, OUT TArray<uint8_t>& CostFields)
{
	ensure(World);

	FVector HitLocation;
	FHitResult OutHit;
	for (int y = 0; y < GRIDSIZE.Y; ++y)
	{
		for (int x = 0; x < GRIDSIZE.X; ++x)
		{
			FVector RayStart = COST_TRACE_Y_START + FVector(x * CELL_SIZE + CELL_SIZE / 2, y * CELL_SIZE + CELL_SIZE / 2, 0.f);
			FVector RayEnd = RayStart + COST_TRACE_DIRECTION;
			FCollisionQueryParams CollisionQueryParams(SCENE_QUERY_STAT(ClickableTrace), true);
			bool bLineTraceObstructed = World->LineTraceSingleByChannel(OutHit, RayStart, RayEnd, ECollisionChannel::ECC_WorldStatic /* , = FCollisionQueryParams::DefaultQueryParam, = FCollisionResponseParams::DefaultResponseParam */);

			CostFields[y * GRIDSIZE.X + x] = bLineTraceObstructed ? UINT8_MAX : 1;
		}
	}
	return true;
}

void DoFlowTiles(UWorld* World)
{
	ensure(World);
	Debug::pWorld = World;
	TArray<uint8_t> CostFields;
	CostFields.Init(1, GRIDSIZE.X * GRIDSIZE.Y);
	CalculateCostFields(World, CostFields);

	Debug::Draw(World, CostFields);

	Debug::DrawCoords();

	FIntVector2 Goal;
	{
		TArray<AActor*> FoundGoals;
		UGameplayStatics::GetAllActorsWithTag(World, "Goal", FoundGoals);
		check(FoundGoals.Num() == 1 && IsValid(FoundGoals[0]));
		AActor* GoalActor = FoundGoals[0];
		FVector GoalVector = GoalActor->GetActorLocation();
		Goal.X = (int)(GoalVector.X) / CELL_SIZE; // TODO use round?
		Goal.Y = (int)(GoalVector.Y) / CELL_SIZE;
		UE_LOG(LogUE5TopDownARPG, Log, TEXT("Found Goal at [%d, %d]"), Goal.X, Goal.Y);
	}

	Debug::DrawBox(Goal);
	std::queue<FIntVector2> WaveFront;
	WaveFront.push(Goal);
	TArray<IntegrationField> IntegrationFields;
	IntegrationFields.Init({ FLT_MAX, 0, 0 }, GRIDSIZE.X * GRIDSIZE.Y);
	// runLos()
	// remove after runLos() is implemented
	IntegrationFields[Goal.Y * GRIDSIZE.X + Goal.X].IntegratedCost = 0;

	Eikonal::PropagateWave(CostFields, IntegrationFields, WaveFront, Eikonal::VisitCell_LOS, Goal);

	//Debug::Draw(World, IntegrationFields);

	//TArray<FlowField> FlowFields;
	//FlowFields.Init({Dirs::EDirection(), false, 0}, GRIDSIZE.X * GRIDSIZE.Y); // TODO optimize: can we omit constructing these?
	//std::queue<int> Sources; // TODO optimize
	////Sources.push(25 * GRIDSIZE.X + 27);
	//Sources.push(2 * GRIDSIZE.X + 2);
	//Sources.push(27 * GRIDSIZE.X + 2);
	//Sources.push(2 * GRIDSIZE.X + 27);
	////Sources.push(10 * GRIDSIZE.X + 20);
	//CalculateFlowFields(IntegrationFields, Sources, FlowFields);

	//Debug::Draw(World, FlowFields);
}

// TESTS

bool Test_BresenhamsRay2D(UWorld* World)
{
	return false;
}


AUE5TopDownARPGPlayerController::AUE5TopDownARPGPlayerController()
{
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;
	CachedDestination = FVector::ZeroVector;
	FollowTime = 0.f;
}

void AUE5TopDownARPGPlayerController::OnPlayerDied()
{
	AUE5TopDownARPGHUD* HUD = Cast<AUE5TopDownARPGHUD>(GetHUD());
	if (IsValid(HUD))
	{
		HUD->ShowEndGameScreen();
	}
}

void AUE5TopDownARPGPlayerController::BeginPlay()
{
	// Call the base class  
	Super::BeginPlay();

	//Add Input Mapping Context
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->AddMappingContext(DefaultMappingContext, 0);
	}

	UWorld* World = GetWorld();
	ensure(World);

	DoFlowTiles(World);
}

void AUE5TopDownARPGPlayerController::SetupInputComponent()
{
	// set up gameplay key bindings
	Super::SetupInputComponent();

	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = CastChecked<UEnhancedInputComponent>(InputComponent))
	{
		// Setup mouse input events
		EnhancedInputComponent->BindAction(SetDestinationClickAction, ETriggerEvent::Started, this, &AUE5TopDownARPGPlayerController::OnInputStarted);
		EnhancedInputComponent->BindAction(SetDestinationClickAction, ETriggerEvent::Triggered, this, &AUE5TopDownARPGPlayerController::OnSetDestinationTriggered);
		EnhancedInputComponent->BindAction(SetDestinationClickAction, ETriggerEvent::Completed, this, &AUE5TopDownARPGPlayerController::OnSetDestinationReleased);
		EnhancedInputComponent->BindAction(SetDestinationClickAction, ETriggerEvent::Canceled, this, &AUE5TopDownARPGPlayerController::OnSetDestinationReleased);

		EnhancedInputComponent->BindAction(ActivateAbilityAction, ETriggerEvent::Started, this, &AUE5TopDownARPGPlayerController::OnActivateAbilityStarted);

		// Setup touch input events
		EnhancedInputComponent->BindAction(SetDestinationTouchAction, ETriggerEvent::Started, this, &AUE5TopDownARPGPlayerController::OnInputStarted);
		EnhancedInputComponent->BindAction(SetDestinationTouchAction, ETriggerEvent::Triggered, this, &AUE5TopDownARPGPlayerController::OnTouchTriggered);
		EnhancedInputComponent->BindAction(SetDestinationTouchAction, ETriggerEvent::Completed, this, &AUE5TopDownARPGPlayerController::OnTouchReleased);
		EnhancedInputComponent->BindAction(SetDestinationTouchAction, ETriggerEvent::Canceled, this, &AUE5TopDownARPGPlayerController::OnTouchReleased);
	}
}

void AUE5TopDownARPGPlayerController::OnInputStarted()
{
	StopMovement();
}

// Triggered every frame when the input is held down
void AUE5TopDownARPGPlayerController::OnSetDestinationTriggered()
{
	// We flag that the input is being pressed
	FollowTime += GetWorld()->GetDeltaSeconds();
	
	// We look for the location in the world where the player has pressed the input
	FHitResult Hit;
	bool bHitSuccessful = false;
	if (bIsTouch)
	{
		bHitSuccessful = GetHitResultUnderFinger(ETouchIndex::Touch1, ECollisionChannel::ECC_Visibility, true, Hit);
	}
	else
	{
		bHitSuccessful = GetHitResultUnderCursor(ECollisionChannel::ECC_Visibility, true, Hit);
	}

	// If we hit a surface, cache the location
	if (bHitSuccessful)
	{
		CachedDestination = Hit.Location;
	}
	
	// Move towards mouse pointer or touch
	APawn* ControlledPawn = GetPawn();
	if (ControlledPawn != nullptr)
	{
		FVector WorldDirection = (CachedDestination - ControlledPawn->GetActorLocation()).GetSafeNormal();
		ControlledPawn->AddMovementInput(WorldDirection, 1.0, false);
	}
}

void AUE5TopDownARPGPlayerController::OnSetDestinationReleased()
{
	// If it was a short press
	if (FollowTime <= ShortPressThreshold)
	{
		// We move there and spawn some particles
		UAIBlueprintHelperLibrary::SimpleMoveToLocation(this, CachedDestination);
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, FXCursor, CachedDestination, FRotator::ZeroRotator, FVector(1.f, 1.f, 1.f), true, true, ENCPoolMethod::None, true);
	}

	FollowTime = 0.f;
}

// Triggered every frame when the input is held down
void AUE5TopDownARPGPlayerController::OnTouchTriggered()
{
	bIsTouch = true;
	OnSetDestinationTriggered();
}

void AUE5TopDownARPGPlayerController::OnTouchReleased()
{
	bIsTouch = false;
	OnSetDestinationReleased();
}

void AUE5TopDownARPGPlayerController::OnActivateAbilityStarted()
{
	UE_LOG(LogUE5TopDownARPG, Log, TEXT("OnActivateAbilityStarted"));

	AUE5TopDownARPGCharacter* ARPGCharacter = Cast<AUE5TopDownARPGCharacter>(GetPawn());
	if (IsValid(ARPGCharacter))
	{
		FHitResult Hit;
		bool bHitSuccessful = false;
		if (bIsTouch)
		{
			bHitSuccessful = GetHitResultUnderFinger(ETouchIndex::Touch1, ECollisionChannel::ECC_Visibility, true, Hit);
		}
		else
		{
			bHitSuccessful = GetHitResultUnderCursor(ECollisionChannel::ECC_Visibility, true, Hit);
		}

		// If we hit a surface, cache the location
		if (bHitSuccessful)
		{
			ARPGCharacter->ActivateAbility(Hit.Location);
		}
	}
}