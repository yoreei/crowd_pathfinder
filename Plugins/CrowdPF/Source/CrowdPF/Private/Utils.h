#pragma once

#include <cstdint>
#include "CoreMinimal.h"
#include "CrowdPFImpl.h"
// #include "CrowdPFImpl.h"
#include <map>

#define BREAK_IF_EQUAL(current, at) if ((current) == (at)) { __debugbreak(); }

const int ApplyDir(const int Idx, const FIntVector2& Dir) {
	int Result = Idx + Dir.Y * GRIDSIZE.X + Dir.X;
	return Result;
}

bool IsInGrid(int X, int Y)
{
	return X >= 0 && X < GRIDSIZE.X && Y >= 0 && Y < GRIDSIZE.Y;
}

bool IsInGrid(const FIntVector2& Cell) {
	return IsInGrid(Cell.X, Cell.Y);
}

bool IsInGrid(int Idx)
{
	int X = Idx % GRIDSIZE.X;
	int Y = Idx / GRIDSIZE.X;
	return IsInGrid(X, Y);
}

bool IsWall(const TArray<uint8_t>& CostFields, const FIntVector2& Cell)
{
	return CostFields[Cell.Y * GRIDSIZE.X + Cell.X] == UINT8_MAX;
}

FIntVector2 ToFIntVector2(int LinearIdx)
{
	return FIntVector2(
		LinearIdx % GRIDSIZE.X,
		LinearIdx / GRIDSIZE.X
	);
}

int ToLinearIdx(FIntVector2 IntVector2)
{
	return IntVector2.Y * GRIDSIZE.X + IntVector2.X;
}

FVector2D ToVector2D(const FIntVector2& IntVector2)
{
	FVector2D Result;
	Result.X = static_cast<float>(IntVector2.X);
	Result.Y = static_cast<float>(IntVector2.Y);
	return Result;
};

bool IsValidIdx(int Idx)
{
	return Idx >= 0 && Idx < GRIDSIZE.X * GRIDSIZE.Y;
}

FVector addZ(FVector2D Vect, float Z)
{
	return FVector(Vect.X, Vect.Y, Z);
}