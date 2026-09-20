// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MeshAnalyticsSettings.generated.h"

/** Editor settings used by the Mesh Analytics tool. */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Asset Analytics"))
class ASSETANALYTICS_API UMeshAnalyticsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Sets where these options appear in Editor Preferences. */
	UMeshAnalyticsSettings();

	/** Skips mesh assets that have no relevant project references. */
	UPROPERTY(config, EditAnywhere, Category = "Asset Filters")
	bool bIgnoreUnreferencedAssets = true;

	/** Ignores references coming from the Developers folder. */
	UPROPERTY(config, EditAnywhere, Category = "Asset Filters")
	bool bIgnoreDeveloperReferences = true;
	
	/** Content Browser folders that will be searched for mesh assets. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics", meta = (ContentDir))
	TArray<FDirectoryPath> SearchFolders;

	/** Includes Static Mesh assets in Mesh Analytics. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Asset Types")
	bool bStaticMeshes = true;

	/** Includes Skeletal Mesh assets in Mesh Analytics. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Asset Types")
	bool bSkeletalMeshes = true;

	/** Includes the package size on disk in bytes. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bPackageDiskSize = true;

	/** Includes the LOD count in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bLODCount = true;

	/** Includes the static mesh LOD group in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bLODGroup = true;

	/** Includes collision complexity and the simple collision primitive count. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bCollisionInfo = true;

	/** Includes the estimated physics resource size in megabytes. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Physics Size (Loads Assets Missing Registry Data)"))
	bool bPhysicsSize = true;

	/** Includes the LOD 0 triangle count in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bTriangleCount = true;

	/** Includes Nanite status, input triangle count, and static-mesh fallback percentage. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Nanite Info"))
	bool bNaniteInfo = true;

	/** Includes the material count in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Material Count (Needs Asset Loading for Skeletal Meshes)"))
	bool bMaterialCount = true;

	/** Includes the UV channel count in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "UV Channel Count (Needs Asset Loading for Skeletal Meshes)"))
	bool bUVChannelCount = true;

	/** Includes the maximum bounds length in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Max Bounds Length (Needs Asset Loading for Skeletal Meshes)"))
	bool bMaxBoundsLength = true;

	/** Includes the complex collision vertex count in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Complex Collision Info (Needs Asset Loading)"))
	bool bComplexCollisionInfo = false;
	
	/** Includes the unique texture count in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Texture Count (Needs Asset Loading)"))
	bool bTextureCount = false;

	/** Includes the maximum texture resolution in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Max Texture Resolution (Needs Asset Loading)"))
	bool bMaxTextureResolution = false;

	/** Includes the Static Mesh lightmap resolution in the generated CSV. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Lightmap Resolution (Needs Asset Loading)"))
	bool bLightmapResolution = false;
};
