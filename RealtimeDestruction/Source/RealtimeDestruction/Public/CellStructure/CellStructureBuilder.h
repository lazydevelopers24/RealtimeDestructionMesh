// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CellStructure/CellStructureTypes.h"

namespace UE::Geometry
{
	class FDynamicMesh3;
}

class UWorld;

class REALTIMEDESTRUCTION_API FCellStructureBuilder
{
public:
	FCellStructureBuilder() = default;
	~FCellStructureBuilder() = default;

	bool BuildFromMesh(const UE::Geometry::FDynamicMesh3& Mesh,
		const FCellStructureSettings& Settings,
		FCellStructureData& OutData,
		UWorld* World = nullptr,
		bool bValidate = false,
		const FTransform& DebugTransform = FTransform::Identity) const;

	bool ValidateCellStructureData(const UE::Geometry::FDynamicMesh3& Mesh,
		const FCellStructureSettings& Settings,
		const FCellStructureData& Data,
		UWorld* World = nullptr,
		int32 MaxDrawCount = 128,
		const FTransform& DebugTransform = FTransform::Identity) const;

	void DebugDrawCellStructure(const UE::Geometry::FDynamicMesh3& Mesh,
		const FCellStructureSettings& Settings,
		const FCellStructureData& Data,
		const FCellStructureDebugOptions& DebugOptions,
		UWorld* World,
		const FTransform& DebugTransform = FTransform::Identity) const;
};
