// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AssetAnalyticsSettings.generated.h"

/** Editor settings used by the Asset Analytics reports. */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Asset Analytics"))
class ASSETANALYTICS_API UAssetAnalyticsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Sets where these options appear in Editor Preferences. */
	UAssetAnalyticsSettings();

#if WITH_EDITOR
	/** Returns the memory-use guidance shown at the top of the settings page. */
	virtual FText GetSectionDescription() const override;
#endif

	/** Skips assets that have no relevant project references. Used by every report; Material Analytics also leaves such instances out of the instance count. */
	UPROPERTY(config, EditAnywhere, Category = "General")
	bool bIgnoreUnreferencedAssets = true;

	/** Ignores references coming from the Developers folder when checking assets and material instances. Used by every report. */
	UPROPERTY(config, EditAnywhere, Category = "General")
	bool bIgnoreDeveloperReferences = true;

	/**
	 * Number of assets processed per editor tick in every report. Loaded assets are unloaded after each batch,
	 * so higher values finish faster but raise peak memory use.
	 */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (ClampMin = "1", UIMin = "1", UIMax = "500"))
	int32 AssetsPerTick = 50;

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

	/** Includes the estimated physics resource size in megabytes. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Physics Size (Loads Assets Missing Registry Data)"))
	bool bPhysicsSize = false;

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

	/** Content Browser folders that will be searched for master materials. */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics", meta = (ContentDir))
	TArray<FDirectoryPath> MaterialSearchFolders;

	/** Includes the package size on disk in bytes. */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Package Disk Size"))
	bool bMaterialPackageDiskSize = true;

	/** Includes the number and list of instances below the material, including instances of instances. */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Instance Count"))
	bool bMaterialInstanceCounts = true;

	/**
	 * Lists, for each material usage (vertex factory) type, the assets that use the material or any of its counted
	 * instances. Only users visible in the Asset Registry are found; other usage types are written with null users,
	 * and actor-based types such as water are recognised in World Partition levels only.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Vertex Factory Counts (Registry Only)"))
	bool bMaterialUserTypeCounts = true;

	/**
	 * Adds whether each material usage flag is enabled on the master, next to its users, so unused or missing
	 * usages stand out. Each master material is loaded and unloaded again after its batch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Usage Flags (Needs Asset Loading)"))
	bool bMaterialUsageFlags = false;
};
