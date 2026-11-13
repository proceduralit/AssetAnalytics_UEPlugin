// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
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
	 * Number of assets processed in each batch in every report. Loaded assets are unloaded after each batch,
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

	/** Includes the LOD count in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bLODCount = true;

	/** Includes the static mesh LOD group in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bLODGroup = true;

	/** Includes collision complexity and the simple collision primitive count. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bCollisionInfo = true;

	/** Includes the LOD 0 triangle count in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns")
	bool bTriangleCount = true;

	/** Includes the material slot count and the materials the mesh references. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Material Count (Loads Skeletal Meshes)"))
	bool bMaterialCount = true;

	/** Includes the UV channel count in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "UV Channel Count (Loads Skeletal Meshes)"))
	bool bUVChannelCount = true;

	/** Includes the maximum bounds length in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Max Bounds Length (Loads Skeletal Meshes)"))
	bool bMaxBoundsLength = true;

	/** Includes the estimated physics resource size; the report page shows it in megabytes. UE 4.27 saves no registry data for it, so meshes are loaded. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Physics Size (Loads Meshes)"))
	bool bPhysicsSize = false;

	/** Includes the complex collision vertex count in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Complex Collision Info (Loads Static Meshes)"))
	bool bComplexCollisionInfo = false;
	
	/** Includes the unique textures used by the mesh's materials, with their count. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Texture Count (Loads Meshes)"))
	bool bTextureCount = false;

	/** Includes the maximum texture resolution in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Max Texture Resolution (Loads Meshes)"))
	bool bMaxTextureResolution = false;

	/** Includes the Static Mesh lightmap resolution in the Mesh Analytics report. */
	UPROPERTY(config, EditAnywhere, Category = "Mesh Analytics|Columns", meta = (DisplayName = "Lightmap Resolution (Loads Static Meshes)"))
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
	 * instances. Only users visible in the Asset Registry are found; other usage types have no Users list,
	 * and actor-based types such as water are recognised in World Partition levels only.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Vertex Factory Counts (Registry Only)"))
	bool bMaterialUserTypeCounts = true;

	/**
	 * Adds whether each material usage flag is enabled on the master, next to its users, so unused or missing
	 * usages stand out. Each master material is loaded and unloaded again after its batch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Usage Flags (Loads Master Materials)"))
	bool bMaterialUsageFlags = false;

	/**
	 * Loads Blueprint and Cascade particle system users to find the exact usage they need (spline mesh, instanced
	 * static mesh, particle sprites, ...). Skeletal mesh users are checked for morph targets and cloth
	 * without loading them, from Asset Registry data and their package headers. Niagara systems, level
	 * actors and static mesh placements are not resolved. Users are unloaded again after each batch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Vertex Factory Users (Loads User Assets)"))
	bool bMaterialResolveUsersByLoading = false;

	/**
	 * Finds the counted instances that compile their own shader map, how each instance sets the static parameters
	 * and base properties, and the shader count of the master plus one instance per unique static permutation from
	 * the engine's shader map layout. Masters and instances are loaded and unloaded after
	 * each batch.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Analytics|Columns", meta = (DisplayName = "Shader Permutations (Loads Master Materials and Instances)"))
	bool bMaterialShaderPermutations = false;
};
