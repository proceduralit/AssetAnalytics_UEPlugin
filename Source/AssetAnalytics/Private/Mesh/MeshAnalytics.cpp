// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Mesh/MeshAnalytics.h"
#include "Core/AssetAnalyticsSettings.h"
#include "Core/AssetAnalyticsPackageUnloader.h"
#include "Core/AssetAnalyticsProgressNotification.h"
#include "Core/AssetAnalyticsReportBridge.h"
#include "Core/AssetAnalyticsUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/Interface_AsyncCompilation.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "ProfilingDebugging/ResourceSize.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "MeshAnalytics"

namespace AssetAnalytics
{
namespace Mesh
{
	/** Everything the report gathers for one static or skeletal mesh. Unknown numbers stay negative or INDEX_NONE. */
	struct FMeshAssetData
	{
		/** Creates an empty report row for InAssetData. */
		explicit FMeshAssetData(const FAssetData& InAssetData)
			: AssetData(InAssetData)
		{
		}

		/** Registry entry of the mesh. */
		FAssetData AssetData;
		/** Whether the mesh is a skeletal mesh rather than a static mesh. */
		bool bIsSkeletalMesh = false;
		/** Size of the mesh's package on disk in bytes. */
		int64 PackageDiskSize = -1;
		/** Memory used by the mesh's physics data in bytes. */
		int64 PhysicsSize = -1;
		/** Number of LODs. */
		int32 LODCount = INDEX_NONE;
		/** LOD group name, or "None" for skeletal meshes and static meshes without one. */
		FString LODGroup = TEXT("None");
		/** Collision trace flag without its CTF_Use prefix, such as SimpleAndComplex. */
		FString CollisionComplexity;
		/** Number of simple collision shapes. */
		int32 SimpleCollisionPrimitives = INDEX_NONE;
		/** Vertices in the complex collision mesh. */
		int32 ComplexCollisionVertices = INDEX_NONE;
		/** Triangles in LOD 0. */
		int32 TriangleCount = INDEX_NONE;
		/** Whether Nanite is enabled on the mesh. */
		bool bNaniteEnabled = false;
		/** Triangles in the Nanite mesh. */
		int32 NaniteTriangles = INDEX_NONE;
		/** Percentage of Nanite triangles kept in the fallback mesh. Skeletal meshes have none. */
		double NaniteFallbackPercent = -1.0;
		/** Number of material slots. */
		int32 MaterialSlotCount = INDEX_NONE;
		/** Object paths of the materials and material instances the mesh's package references, sorted. */
		TArray<FString> MaterialPaths;
		/** Unique textures used by the mesh's materials. */
		int32 TextureCount = INDEX_NONE;
		/** Object paths of those unique textures, sorted. Empty when the mesh was not loaded. */
		TArray<FString> TexturePaths;
		/** Width and height of each texture in TexturePaths, at the same index, as GetTextureResolution returns. */
		TArray<FIntPoint> TextureSizes;
		/** Whether each texture in TexturePaths, at the same index, has Virtual Texture Streaming enabled. */
		TArray<bool> TextureVirtualFlags;
		/** Largest width or height among those textures. */
		int32 MaxTextureResolution = INDEX_NONE;
		/** UV channels in LOD 0. */
		int32 UVChannelCount = INDEX_NONE;
		/** Lightmap resolution set on the mesh. */
		int32 LightmapResolution = INDEX_NONE;
		/** Longest side of the mesh bounds in meters. */
		double MaxBoundsLengthM = -1.0;
	};

	/** JSON writer used for the report file. */
	using FMeshJsonWriter = TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>;

	/** Returns the report name of CollisionTraceFlag, resolving Default through the project's physics settings. */
	static FString GetCollisionComplexityName(ECollisionTraceFlag CollisionTraceFlag)
	{
		if (CollisionTraceFlag == CTF_UseDefault) CollisionTraceFlag = UPhysicsSettings::Get()->DefaultShapeComplexity;
		switch (CollisionTraceFlag)
		{
		case CTF_UseSimpleAndComplex: return TEXT("SimpleAndComplex");
		case CTF_UseSimpleAsComplex: return TEXT("SimpleAsComplex");
		case CTF_UseComplexAsSimple: return TEXT("ComplexAsSimple");
		default: return TEXT("None");
		}
	}

	/**
	 * Returns the width and height of Texture: the imported size for 2D textures, otherwise the surface size. When
	 * Maximum Texture Size is set, both sides are halved until the longer one fits, as the engine drops mips.
	 */
	static FIntPoint GetTextureResolution(const UTexture& Texture)
	{
		FIntPoint Resolution(
			FMath::RoundToInt(Texture.GetSurfaceWidth()),
			FMath::RoundToInt(Texture.GetSurfaceHeight()));
		if (const UTexture2D* Texture2D = Cast<UTexture2D>(&Texture))
		{
			Resolution = Texture2D->GetImportedSize();
		}
		if (Texture.MaxTextureSize > 0)
		{
			while (FMath::Max(Resolution.X, Resolution.Y) > Texture.MaxTextureSize)
			{
				Resolution.X = FMath::Max(Resolution.X / 2, 1);
				Resolution.Y = FMath::Max(Resolution.Y / 2, 1);
			}
		}
		return Resolution;
	}

	/** Fills Item with the material and texture data enabled in Settings. */
	static void GatherMaterialAnalyticsData(
		const TArray<UMaterialInterface*>& Materials,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		if (Settings.bMaterialCount) Item.MaterialSlotCount = Materials.Num();

		const bool bCollectTextureCount = Settings.bTextureCount;
		const bool bCollectTextureResolution = Settings.bMaxTextureResolution;
		if (!bCollectTextureCount && !bCollectTextureResolution) return;

		TSet<UTexture*> UniqueTextures;
		for (const UMaterialInterface* Material : Materials)
		{
			if (Material == nullptr) continue;

			TArray<UTexture*> MaterialTextures;
			Material->GetUsedTextures(MaterialTextures);
			for (UTexture* Texture : MaterialTextures)
			{
				if (Texture != nullptr) UniqueTextures.Add(Texture);
			}
		}
		Item.TextureCount = UniqueTextures.Num();
		if (bCollectTextureCount)
		{
			// Sorted by path, with each texture's size and virtual texture flag kept at the same index as its path.
			TArray<UTexture*> SortedTextures = UniqueTextures.Array();
			SortedTextures.Sort([](const UTexture& Left, const UTexture& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
			for (const UTexture* Texture : SortedTextures)
			{
				Item.TexturePaths.Add(Texture->GetPathName());
				Item.TextureSizes.Add(GetTextureResolution(*Texture));
				// The texture's own setting; it only takes effect when the project enables virtual textures.
				Item.TextureVirtualFlags.Add(Texture->VirtualTextureStreaming != 0);
			}
		}

		if (!bCollectTextureResolution) return;
		int32 MaxTextureResolution = 0;
		for (const UTexture* Texture : UniqueTextures)
		{
			const FIntPoint TextureResolution = GetTextureResolution(*Texture);
			MaxTextureResolution = FMath::Max(MaxTextureResolution, FMath::Max(TextureResolution.X, TextureResolution.Y));
		}
		Item.MaxTextureResolution = MaxTextureResolution;
	}

	/** Fills Item with the enabled skeletal mesh data. */
	static void GatherSkeletalMeshData(
		USkeletalMesh& SkeletalMesh,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		if (Settings.bLODCount) Item.LODCount = SkeletalMesh.GetLODNum();
		if (Settings.bPhysicsSize && Item.PhysicsSize < 0)
		{
			Item.PhysicsSize = 0;
			if (UBodySetup* MeshBodySetup = SkeletalMesh.GetBodySetup())
			{
				FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
				MeshBodySetup->GetResourceSizeEx(EstimatedSize);
				Item.PhysicsSize += static_cast<int64>(EstimatedSize.GetTotalMemoryBytes());
			}
			if (UPhysicsAsset* PhysicsAsset = SkeletalMesh.GetPhysicsAsset())
			{
				FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
				PhysicsAsset->GetResourceSizeEx(EstimatedSize);
				Item.PhysicsSize += static_cast<int64>(EstimatedSize.GetTotalMemoryBytes());
			}
		}

		const bool bGatherMaterialData =
			Settings.bMaterialCount ||
			Settings.bTextureCount ||
			Settings.bMaxTextureResolution;
		if (bGatherMaterialData)
		{
			TArray<UMaterialInterface*> Materials;
			for (const FSkeletalMaterial& SkeletalMaterial : SkeletalMesh.GetMaterials())
			{
				Materials.Add(SkeletalMaterial.MaterialInterface);
			}
			GatherMaterialAnalyticsData(Materials, Settings, Item);
		}
		if (Settings.bMaxBoundsLength)
		{
			const FVector BoundsSize = SkeletalMesh.GetBounds().BoxExtent * 2.0f;
			Item.MaxBoundsLengthM = FMath::Max(BoundsSize.GetMax(), 1.0f) / 100.0;
		}
		if (Settings.bUVChannelCount)
		{
			const FSkeletalMeshRenderData* RenderData = SkeletalMesh.GetResourceForRendering();
			if (RenderData != nullptr && RenderData->LODRenderData.Num() > 0)
			{
				const FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[0];
				Item.UVChannelCount = static_cast<int32>(LODRenderData.GetNumTexCoords());
			}
		}
	}

	/** Fills Item with the enabled static mesh data. */
	static void GatherStaticMeshData(
		UStaticMesh& StaticMesh,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		if (Settings.bLODCount) Item.LODCount = StaticMesh.GetNumLODs();
		if (Settings.bLODGroup) Item.LODGroup = StaticMesh.GetLODGroup().ToString();
		if (Settings.bPhysicsSize && Item.PhysicsSize < 0)
		{
			Item.PhysicsSize = 0;
			if (UBodySetup* BodySetup = StaticMesh.GetBodySetup())
			{
				FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
				BodySetup->GetResourceSizeEx(EstimatedSize);
				Item.PhysicsSize = static_cast<int64>(EstimatedSize.GetTotalMemoryBytes());
			}
		}
		// Registry values are kept when present, so a mesh reports the same values whether or not it was loaded.
		if (Settings.bCollisionInfo && Item.CollisionComplexity.IsEmpty())
		{
			if (const UBodySetup* BodySetup = StaticMesh.GetBodySetup())
			{
				Item.CollisionComplexity = GetCollisionComplexityName(BodySetup->CollisionTraceFlag);
				Item.SimpleCollisionPrimitives = BodySetup->AggGeom.GetElementCount();
			}
			else
			{
				Item.CollisionComplexity = TEXT("None");
				Item.SimpleCollisionPrimitives = 0;
			}
		}
		if (Settings.bComplexCollisionInfo)
		{
			FTriMeshCollisionData CollisionData;
			Item.ComplexCollisionVertices = StaticMesh.GetPhysicsTriMeshData(&CollisionData, false)
				? CollisionData.Vertices.Num()
				: 0;
		}

		const bool bGatherMaterialData =
			Settings.bMaterialCount ||
			Settings.bTextureCount ||
			Settings.bMaxTextureResolution;
		if (bGatherMaterialData)
		{
			TArray<UMaterialInterface*> Materials;
			for (const FStaticMaterial& StaticMaterial : StaticMesh.GetStaticMaterials())
			{
				Materials.Add(StaticMaterial.MaterialInterface);
			}
			GatherMaterialAnalyticsData(Materials, Settings, Item);
		}
		if (Settings.bMaxBoundsLength && Item.MaxBoundsLengthM < 0.0)
		{
			const FVector BoundsSize = StaticMesh.GetBounds().BoxExtent * 2.0f;
			Item.MaxBoundsLengthM = FMath::Max(BoundsSize.GetMax(), 1.0f) / 100.0;
		}
		if (Settings.bLightmapResolution) Item.LightmapResolution = StaticMesh.GetLightMapResolution();
	}

	/**
	 * Lists the materials and material instances MeshData's package references, without loading anything.
	 * This covers the material slots and any other material the mesh saves, such as LOD or overlay materials.
	 * Returns: Sorted object paths.
	 */
	static TArray<FString> FindReferencedMaterials(IAssetRegistry& AssetRegistry, const FAssetData& MeshData)
	{
		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(
			MeshData.PackageName,
			Dependencies,
			UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));

		TArray<FString> MaterialPaths;
		for (const FName Dependency : Dependencies)
		{
			TArray<FAssetData> DependencyAssets;
			AssetRegistry.GetAssetsByPackageName(Dependency, DependencyAssets);
			for (const FAssetData& DependencyAsset : DependencyAssets)
			{
				if (DependencyAsset.IsInstanceOf(UMaterialInterface::StaticClass()))
				{
					MaterialPaths.Add(DependencyAsset.GetObjectPathString());
				}
			}
		}
		MaterialPaths.Sort();
		return MaterialPaths;
	}

	/** Fills Item with values cached in the Asset Registry. */
	static void GatherAssetRegistryData(
		IAssetRegistry& AssetRegistry,
		const FAssetData& AssetData,
		const UAssetAnalyticsSettings& Settings,
		FMeshAssetData& Item)
	{
		Item.bIsSkeletalMesh = AssetData.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName();
		if (Settings.bPackageDiskSize)
		{
			FAssetPackageData PackageData;
			if (AssetRegistry.TryGetAssetPackageData(AssetData.PackageName, PackageData) ==
				UE::AssetRegistry::EExists::Exists)
			{
				Item.PackageDiskSize = PackageData.DiskSize;
			}
		}
		if (Settings.bPhysicsSize) AssetData.GetTagValue(TEXT("PhysicsSize"), Item.PhysicsSize);
		if (Settings.bLODCount) AssetData.GetTagValue(TEXT("LODs"), Item.LODCount);
		if (Settings.bTriangleCount) AssetData.GetTagValue(TEXT("Triangles"), Item.TriangleCount);
		if (Settings.bNaniteInfo)
		{
			AssetData.GetTagValue(TEXT("NaniteEnabled"), Item.bNaniteEnabled);
			AssetData.GetTagValue(TEXT("NaniteTriangles"), Item.NaniteTriangles);
			if (!Item.bIsSkeletalMesh)
			{
				AssetData.GetTagValue(TEXT("NaniteFallbackPercent"), Item.NaniteFallbackPercent);
			}
		}
		if (Settings.bMaterialCount) Item.MaterialPaths = FindReferencedMaterials(AssetRegistry, AssetData);

		// Skeletal meshes have no LOD group and do not store the remaining tags.
		if (Item.bIsSkeletalMesh) return;

		if (Settings.bLODGroup) AssetData.GetTagValue(TEXT("LODGroup"), Item.LODGroup);
		if (Settings.bMaterialCount) AssetData.GetTagValue(TEXT("Materials"), Item.MaterialSlotCount);
		if (Settings.bUVChannelCount) AssetData.GetTagValue(TEXT("UVChannels"), Item.UVChannelCount);
		if (Settings.bCollisionInfo)
		{
			// The tag holds the flag's enum name, such as CTF_UseDefault, or is empty without a body setup.
			// It is mapped like a loaded mesh, so Default is resolved through the physics settings.
			FString CollisionTraceFlagText;
			if (AssetData.GetTagValue(TEXT("CollisionComplexity"), CollisionTraceFlagText))
			{
				ECollisionTraceFlag CollisionTraceFlag = CTF_MAX;
				if (CollisionTraceFlagText == TEXT("CTF_UseDefault")) CollisionTraceFlag = CTF_UseDefault;
				else if (CollisionTraceFlagText == TEXT("CTF_UseSimpleAndComplex")) CollisionTraceFlag = CTF_UseSimpleAndComplex;
				else if (CollisionTraceFlagText == TEXT("CTF_UseSimpleAsComplex")) CollisionTraceFlag = CTF_UseSimpleAsComplex;
				else if (CollisionTraceFlagText == TEXT("CTF_UseComplexAsSimple")) CollisionTraceFlag = CTF_UseComplexAsSimple;
				Item.CollisionComplexity = GetCollisionComplexityName(CollisionTraceFlag);
			}
			AssetData.GetTagValue(TEXT("CollisionPrims"), Item.SimpleCollisionPrimitives);
		}
		if (Settings.bMaxBoundsLength)
		{
			FString ApproximateSize;
			if (AssetData.GetTagValue(TEXT("ApproxSize"), ApproximateSize))
			{
				TArray<FString> Dimensions;
				ApproximateSize.ParseIntoArray(Dimensions, TEXT("x"));
				if (Dimensions.Num() == 3)
				{
					const double MaxBoundsLengthCm = FMath::Max3(
						FCString::Atod(*Dimensions[0]),
						FCString::Atod(*Dimensions[1]),
						FCString::Atod(*Dimensions[2]));
					Item.MaxBoundsLengthM = FMath::Max(MaxBoundsLengthCm, 1.0) / 100.0;
				}
			}
		}
	}

	/** Returns whether any enabled column can load a mesh, so batches know whether to track loaded packages. */
	static bool CanLoadAssets(const UAssetAnalyticsSettings& Settings)
	{
		return Settings.bPhysicsSize ||
			Settings.bTextureCount ||
			Settings.bMaxTextureResolution ||
			Settings.bMaterialCount ||
			Settings.bUVChannelCount ||
			Settings.bMaxBoundsLength ||
			Settings.bComplexCollisionInfo ||
			Settings.bLightmapResolution;
	}

	/**
	 * Returns whether the enabled columns read registry data outside the search folders: referencers for skipping
	 * unreferenced meshes, and referenced materials. The run then waits for the registry's startup scan.
	 */
	static bool NeedsFullAssetRegistry(const UAssetAnalyticsSettings& Settings)
	{
		return Settings.bIgnoreUnreferencedAssets || Settings.bMaterialCount;
	}

	/** Returns whether the enabled columns require loading AssetData. */
	static bool NeedsAssetLoading(
		const FAssetData& AssetData,
		const UAssetAnalyticsSettings& Settings)
	{
		const bool bIsSkeletalMesh = AssetData.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName();
		int64 CachedPhysicsSize = -1;
		if (Settings.bPhysicsSize && !AssetData.GetTagValue(TEXT("PhysicsSize"), CachedPhysicsSize)) return true;
		if (Settings.bTextureCount || Settings.bMaxTextureResolution) return true;
		if (bIsSkeletalMesh)
		{
			return Settings.bMaterialCount || Settings.bUVChannelCount || Settings.bMaxBoundsLength;
		}
		return Settings.bComplexCollisionInfo || Settings.bLightmapResolution;
	}

	/**
	 * Fills a report row from registry data and an optional loaded MeshAsset.
	 * Returns: The completed report row.
	 */
	static FMeshAssetData MakeMeshAssetData(
		IAssetRegistry& AssetRegistry,
		const FAssetData& AssetData,
		UObject* MeshAsset,
		const UAssetAnalyticsSettings& Settings)
	{
		FMeshAssetData Item(AssetData);
		GatherAssetRegistryData(AssetRegistry, AssetData, Settings, Item);
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(MeshAsset)) GatherSkeletalMeshData(*SkeletalMesh, Settings, Item);
		else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(MeshAsset)) GatherStaticMeshData(*StaticMesh, Settings, Item);
		return Item;
	}

	/**
	 * Returns whether MeshAsset is still building its render data, such as Nanite data after a cache miss.
	 * Reading a mesh while it builds blocks the editor until the build finishes.
	 */
	static bool IsMeshBuilding(UObject* MeshAsset)
	{
		const IInterface_AsyncCompilation* AsyncCompilation = Cast<IInterface_AsyncCompilation>(MeshAsset);
		return AsyncCompilation != nullptr && AsyncCompilation->IsCompiling();
	}

	/** Gathers the Mesh Analytics report over several editor ticks and saves it when done. */
	class FMeshAnalyticsRunner
	{
	public:
		/** Starts gathering unless a report is already running. */
		void Start()
		{
			if (TickerHandle.IsValid()) return;
			ResetReportData();
			PackageUnloader.Reset();
			ProgressNotification.Show(
				LOCTEXT("PreparingMeshAnalytics", "Preparing Mesh Analytics..."),
				LOCTEXT("CancelMeshAnalyticsTooltip", "Stop gathering mesh analytics."),
				FSimpleDelegate::CreateRaw(this, &FMeshAnalyticsRunner::Cancel));

			// Work is split into small steps run on separate ticks, so the editor repaints the notification in between.
			Step = ERunnerStep::ShowNotification;
			StepStartSeconds = FPlatformTime::Seconds();
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FMeshAnalyticsRunner::Tick));
		}

		/** Stops gathering and throws away unfinished results. */
		void Stop()
		{
			AbortRun();
			ProgressNotification.Dismiss();
		}

	private:
		/** Stages of a report run. Each Tick performs one stage, or part of ProcessMeshes. */
		enum class ERunnerStep : uint8
		{
			/** Waits briefly so the notification is visible before any blocking work. */
			ShowNotification,
			/** Waits without blocking until the Asset Registry has finished its startup scan. */
			WaitForAssetRegistry,
			/** Scans the search folders and lists the mesh assets. */
			FindMeshes,
			/** Fills report rows within a per-tick time budget. */
			ProcessMeshes,
			/** Waits without blocking until the batch's meshes finish building, then fills their rows. */
			WaitForMeshBuilds,
			/** Unloads the packages loaded by the finished batch. */
			UnloadPackages
		};

		/** Updates the notification with the current asset count and the last asset processed. */
		void UpdateProgressNotification() const
		{
			const int32 ProgressPercent = PendingAssets.Num() > 0
				? FMath::RoundToInt(static_cast<float>(NextAssetIndex) / PendingAssets.Num() * 100.0f)
				: 100;
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("MeshAnalyticsProgress", "Gathering Mesh Analytics - {0} / {1} ({2}%)\n{3}"),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::AsNumber(ProgressPercent),
				FText::FromString(CurrentAssetName)));
		}

		/** Tells the user how many meshes of the batch are still building, naming one of them. */
		void UpdateMeshBuildNotification() const
		{
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("WaitingForMeshBuilds", "Mesh Analytics - {0} / {1}\nWaiting for {2} meshes to finish building Nanite and render data...\n{3}"),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::AsNumber(MeshesWaitingForBuild.Num()),
				FText::FromName(MeshesWaitingForBuild[0].AssetData.AssetName)));
		}

		/** Clears pending assets, finished rows and the batch state. */
		void ResetReportData()
		{
			PendingAssets.Reset();
			AssetItems.Reset();
			MeshesWaitingForBuild.Reset();
			NextAssetIndex = 0;
			AssetsInBatch = 0;
			BatchLoadedAssetCount = 0;
			PackagesBeforeBatch.Reset();
			CurrentAssetName.Reset();
		}

		/** Adds the packages loaded by a batch that was interrupted to the unloader, so they are not left loaded. */
		void TrackUnfinishedBatch()
		{
			if (AssetsInBatch > 0 && BatchLoadedAssetCount > 0) PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
		}

		/** Stops ticking, throws away unfinished results, and unloads the packages the run loaded. */
		void AbortRun()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			TrackUnfinishedBatch();
			ResetReportData();
			PackageUnloader.UnloadReportPackages();
		}

		/** Cancels gathering, clears unfinished results, and updates the notification. */
		void Cancel()
		{
			AbortRun();
			ProgressNotification.Finish(
				LOCTEXT("MeshAnalyticsCancelled", "Mesh Analytics gathering cancelled."),
				SNotificationItem::CS_None);
		}

		/** Scans the configured folders and lists their mesh assets in PendingAssets. */
		void FindMeshAssets(IAssetRegistry& AssetRegistry)
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			FARFilter Filter;
			if (Settings->bStaticMeshes) Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
			if (Settings->bSkeletalMeshes) Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			for (const FDirectoryPath& Directory : Settings->SearchFolders)
			{
				FName PackagePath;
				if (Core::TryGetPackagePath(Directory.Path, PackagePath))
				{
					Filter.PackagePaths.AddUnique(PackagePath);
				}
				else
				{
					UE_LOG(
						LogTemp,
						Warning,
						TEXT("Asset Analytics skipped search folder '%s' because it is invalid or its content root is not mounted."),
						*Directory.Path);
				}
			}
			if (Filter.ClassPaths.Num() == 0 || Filter.PackagePaths.Num() == 0) return;

			TArray<FString> PathsToScan;
			for (const FName PackagePath : Filter.PackagePaths)
			{
				PathsToScan.Add(PackagePath.ToString());
			}
			AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
			AssetRegistry.GetAssets(Filter, PendingAssets);
		}

		/**
		 * Fills the report row of one mesh asset, loading it only for columns that require it.
		 * A loaded mesh that is still building is added to MeshesWaitingForBuild instead, and gets its row once built.
		 * Returns: True when this call loaded the asset from disk.
		 */
		bool ProcessMeshAsset(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings, const FAssetData& AssetData)
		{
			if (Settings.bIgnoreUnreferencedAssets &&
				!Core::HasRelevantReferencer(AssetRegistry, AssetData, Settings.bIgnoreDeveloperReferences)) return false;

			UObject* MeshAsset = nullptr;
			bool bWasAssetLoaded = true;
			if (NeedsAssetLoading(AssetData, Settings))
			{
				bWasAssetLoaded = AssetData.IsAssetLoaded();
				MeshAsset = AssetData.GetAsset();
			}
			if (IsMeshBuilding(MeshAsset)) MeshesWaitingForBuild.Add({ AssetData, MeshAsset });
			else AssetItems.Add(MakeMeshAssetData(AssetRegistry, AssetData, MeshAsset, Settings));
			return !bWasAssetLoaded && MeshAsset != nullptr;
		}

		/**
		 * Starts unloading the finished batch on the next tick when it loaded any meshes.
		 * Returns: True when the unload step was started; false when the batch loaded nothing and was reset.
		 */
		bool TryStartUnloadingBatch()
		{
			if (BatchLoadedAssetCount == 0)
			{
				AssetsInBatch = 0;
				return false;
			}
			// Unloading can take a while, so it gets its own tick and message.
			PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
			Step = ERunnerStep::UnloadPackages;
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("UnloadingMeshPackages", "Mesh Analytics - {0} / {1}\nUnloading {2} packages..."),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::AsNumber(PackageUnloader.GetTrackedPackageCount())));
			return true;
		}

		/**
		 * Fills the rows of waiting meshes whose builds have finished, without waiting for the others.
		 * Once none are left, ends the batch and goes back to processing meshes or unloading the batch.
		 * Returns: True, since the report is still running.
		 */
		bool GatherBuiltMeshes(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings)
		{
			for (int32 WaitingIndex = MeshesWaitingForBuild.Num() - 1; WaitingIndex >= 0; --WaitingIndex)
			{
				const FMeshWaitingForBuild& WaitingMesh = MeshesWaitingForBuild[WaitingIndex];
				// A mesh that is no longer loaded still gets the row its registry data gives.
				UObject* MeshAsset = WaitingMesh.MeshAsset.Get();
				if (IsMeshBuilding(MeshAsset)) continue;
				AssetItems.Add(MakeMeshAssetData(AssetRegistry, WaitingMesh.AssetData, MeshAsset, Settings));
				MeshesWaitingForBuild.RemoveAtSwap(WaitingIndex);
			}
			if (MeshesWaitingForBuild.Num() > 0)
			{
				UpdateMeshBuildNotification();
				return true;
			}

			if (TryStartUnloadingBatch()) return true;
			// The next ProcessMeshes tick completes the report when this was the last batch.
			Step = ERunnerStep::ProcessMeshes;
			UpdateProgressNotification();
			return true;
		}

		/**
		 * Processes mesh assets until the tick time budget is used, so the editor keeps repainting.
		 * AssetsPerTick assets make one batch; when a batch has loaded assets, the next tick unloads them.
		 * Returns: True while the report is still running.
		 */
		bool ProcessMeshesForOneTick(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings)
		{
			const double TickStartSeconds = FPlatformTime::Seconds();
			const int32 AssetsPerBatch = FMath::Max(Settings.AssetsPerTick, 1);
			const bool bLoadsAssets = CanLoadAssets(Settings);
			while (NextAssetIndex < PendingAssets.Num())
			{
				// Snapshot loaded packages so everything this batch loads, including dependencies, can be unloaded afterwards.
				if (bLoadsAssets && AssetsInBatch == 0)
				{
					PackagesBeforeBatch = Core::FReportPackageUnloader::GetLoadedPackages();
					BatchStartSeconds = FPlatformTime::Seconds();
					BatchLoadedAssetCount = 0;
				}

				const FAssetData& AssetData = PendingAssets[NextAssetIndex];
				CurrentAssetName = AssetData.AssetName.ToString();
				if (ProcessMeshAsset(AssetRegistry, Settings, AssetData)) ++BatchLoadedAssetCount;
				++NextAssetIndex;
				++AssetsInBatch;

				const bool bBatchFinished = AssetsInBatch >= AssetsPerBatch || NextAssetIndex >= PendingAssets.Num();
				if (bBatchFinished)
				{
					// The batch stays loaded until its building meshes are gathered, polled each tick so the editor stays responsive.
					if (MeshesWaitingForBuild.Num() > 0)
					{
						Step = ERunnerStep::WaitForMeshBuilds;
						UpdateMeshBuildNotification();
						return true;
					}
					if (TryStartUnloadingBatch()) return true;
				}
				if (FPlatformTime::Seconds() - TickStartSeconds >= TickTimeBudgetSeconds) break;
			}

			if (NextAssetIndex >= PendingAssets.Num())
			{
				TickerHandle.Reset();
				CompleteReport();
				return false;
			}
			UpdateProgressNotification();
			return true;
		}

		/**
		 * Runs the current stage of the report and moves to the next one. Stages that block the editor run on their
		 * own tick after their message is shown, so the notification always says what is happening.
		 * Returns: True while the report is still running.
		 */
		bool Tick(float)
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();

			switch (Step)
			{
			case ERunnerStep::ShowNotification:
				// The notification fades in over a few frames; blocking work before that would hide it.
				if (FPlatformTime::Seconds() - StepStartSeconds < NotificationDisplayDelaySeconds) return true;
				if (NeedsFullAssetRegistry(*Settings))
				{
					Step = ERunnerStep::WaitForAssetRegistry;
					return true;
				}
				Step = ERunnerStep::FindMeshes;
				ProgressNotification.SetText(LOCTEXT("ScanningMeshFolders", "Mesh Analytics\nScanning mesh folders..."));
				return true;

			case ERunnerStep::WaitForAssetRegistry:
				// Polling instead of WaitForCompletion lets the registry keep scanning while the editor stays responsive.
				if (AssetRegistry.IsLoadingAssets())
				{
					ProgressNotification.SetText(LOCTEXT("WaitingForAssetRegistry", "Mesh Analytics\nWaiting for the Asset Registry to finish scanning..."));
					return true;
				}
				Step = ERunnerStep::FindMeshes;
				ProgressNotification.SetText(LOCTEXT("ScanningMeshFolders", "Mesh Analytics\nScanning mesh folders..."));
				return true;

			case ERunnerStep::FindMeshes:
				FindMeshAssets(AssetRegistry);
				if (PendingAssets.Num() == 0)
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				Step = ERunnerStep::ProcessMeshes;
				UpdateProgressNotification();
				return true;

			case ERunnerStep::ProcessMeshes:
				return ProcessMeshesForOneTick(AssetRegistry, *Settings);

			case ERunnerStep::WaitForMeshBuilds:
				return GatherBuiltMeshes(AssetRegistry, *Settings);

			case ERunnerStep::UnloadPackages:
				PackageUnloader.UnloadReportPackages();
				UE_LOG(
					LogTemp,
					Log,
					TEXT("Mesh Analytics loaded and unloaded %d meshes for %d assets in %.2f s."),
					BatchLoadedAssetCount,
					AssetsInBatch,
					FPlatformTime::Seconds() - BatchStartSeconds);
				AssetsInBatch = 0;
				BatchLoadedAssetCount = 0;
				if (NextAssetIndex >= PendingAssets.Num())
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				Step = ERunnerStep::ProcessMeshes;
				UpdateProgressNotification();
				return true;
			}
			return true;
		}

		/** Sorts the results, saves the JSON report, and opens the browser report. */
		void CompleteReport()
		{
			AssetItems.Sort([](const FMeshAssetData& Left, const FMeshAssetData& Right)
			{
				const FString LeftName = Left.AssetData.AssetName.ToString();
				const FString RightName = Right.AssetData.AssetName.ToString();
				if (LeftName != RightName) return LeftName < RightName;
				return Left.AssetData.GetSoftObjectPath().LexicalLess(Right.AssetData.GetSoftObjectPath());
			});

			const FString ReportDirectory = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("AssetAnalytics"));
			IFileManager::Get().MakeDirectory(*ReportDirectory, true);
			const FString JsonPath = FPaths::Combine(ReportDirectory, TEXT("MeshAnalytics.json"));
			if (!FFileHelper::SaveStringToFile(
				BuildJsonData(),
				*JsonPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				ProgressNotification.Finish(
					LOCTEXT("MeshJsonSaveFailed", "The mesh analytics JSON file could not be saved."),
					SNotificationItem::CS_Fail);
				ResetReportData();
				return;
			}

			UE_LOG(LogTemp, Log, TEXT("Asset Analytics saved the Mesh Analytics report to %s"), *FPaths::ConvertRelativePathToFull(JsonPath));
			const FString ReportUrl = Core::GetMeshAnalyticsReportUrl();
			FPlatformProcess::LaunchURL(*ReportUrl, nullptr, nullptr);
			ProgressNotification.Finish(
				FText::Format(
					LOCTEXT("MeshAnalyticsComplete", "Mesh Analytics report generated for {0} assets."),
					FText::AsNumber(AssetItems.Num())),
				SNotificationItem::CS_Success);
			ResetReportData();
		}

		/** Writes a number, or null when it is INDEX_NONE. */
		static void WriteNumberOrNull(FMeshJsonWriter& Writer, const TCHAR* Name, const int32 Value)
		{
			if (Value != INDEX_NONE) Writer.WriteValue(Name, Value);
			else Writer.WriteNull(Name);
		}

		/** Writes a number, or null when it is negative. */
		static void WriteNumberOrNull(FMeshJsonWriter& Writer, const TCHAR* Name, const int64 Value)
		{
			if (Value >= 0) Writer.WriteValue(Name, Value);
			else Writer.WriteNull(Name);
		}

		/** Writes a number, or null when it is negative. */
		static void WriteNumberOrNull(FMeshJsonWriter& Writer, const TCHAR* Name, const double Value)
		{
			if (Value >= 0.0) Writer.WriteValue(Name, Value);
			else Writer.WriteNull(Name);
		}

		/** Writes Paths as a JSON array named Name. */
		static void WritePathArray(FMeshJsonWriter& Writer, const TCHAR* Name, const TArray<FString>& Paths)
		{
			Writer.WriteArrayStart(Name);
			for (const FString& Path : Paths)
			{
				Writer.WriteValue(Path);
			}
			Writer.WriteArrayEnd();
		}

		/**
		 * Writes one mesh entry with the enabled fields. Unknown values are written as null.
		 * Output: Appends a JSON object to Writer, which must be inside the Meshes array.
		 */
		static void WriteMeshJson(FMeshJsonWriter& Writer, const FMeshAssetData& Item, const UAssetAnalyticsSettings& Settings)
		{
			Writer.WriteObjectStart();
			Writer.WriteValue(TEXT("AssetName"), Item.AssetData.AssetName.ToString());
			Writer.WriteValue(TEXT("PackagePath"), Item.AssetData.PackagePath.ToString());
			Writer.WriteValue(TEXT("IsSkeletalMesh"), Item.bIsSkeletalMesh);
			if (Settings.bPackageDiskSize) WriteNumberOrNull(Writer, TEXT("PackageDiskSize"), Item.PackageDiskSize);
			if (Settings.bLODCount) WriteNumberOrNull(Writer, TEXT("LODCount"), Item.LODCount);
			if (Settings.bLODGroup) Writer.WriteValue(TEXT("LODGroup"), Item.LODGroup);
			if (Settings.bCollisionInfo)
			{
				if (!Item.CollisionComplexity.IsEmpty()) Writer.WriteValue(TEXT("CollisionComplexity"), Item.CollisionComplexity);
				else Writer.WriteNull(TEXT("CollisionComplexity"));
				WriteNumberOrNull(Writer, TEXT("SimpleCollisionPrimitives"), Item.SimpleCollisionPrimitives);
			}
			if (Settings.bPhysicsSize) WriteNumberOrNull(Writer, TEXT("PhysicsSize"), Item.PhysicsSize);
			if (Settings.bComplexCollisionInfo) WriteNumberOrNull(Writer, TEXT("ComplexCollisionVertices"), Item.ComplexCollisionVertices);
			if (Settings.bTriangleCount) WriteNumberOrNull(Writer, TEXT("LOD0Triangles"), Item.TriangleCount);
			if (Settings.bNaniteInfo)
			{
				Writer.WriteValue(TEXT("NaniteEnabled"), Item.bNaniteEnabled);
				WriteNumberOrNull(Writer, TEXT("NaniteTriangles"), Item.NaniteTriangles);
				WriteNumberOrNull(Writer, TEXT("NaniteFallbackPercent"), Item.NaniteFallbackPercent);
			}
			if (Settings.bMaterialCount)
			{
				WriteNumberOrNull(Writer, TEXT("MaterialSlots"), Item.MaterialSlotCount);
				// Every material the package references, found in the registry, so it can differ from the slot count.
				WritePathArray(Writer, TEXT("Materials"), Item.MaterialPaths);
			}
			if (Settings.bTextureCount)
			{
				WriteNumberOrNull(Writer, TEXT("UniqueTextures"), Item.TextureCount);
				if (Item.TextureCount != INDEX_NONE)
				{
					WritePathArray(Writer, TEXT("Textures"), Item.TexturePaths);
					// [Width, Height] of each texture in Textures, at the same index.
					Writer.WriteArrayStart(TEXT("TextureSizes"));
					for (const FIntPoint& TextureSize : Item.TextureSizes)
					{
						Writer.WriteArrayStart();
						Writer.WriteValue(TextureSize.X);
						Writer.WriteValue(TextureSize.Y);
						Writer.WriteArrayEnd();
					}
					Writer.WriteArrayEnd();
					// Virtual Texture Streaming setting of each texture in Textures, at the same index.
					Writer.WriteArrayStart(TEXT("TextureVirtual"));
					for (const bool bVirtualTexture : Item.TextureVirtualFlags)
					{
						Writer.WriteValue(bVirtualTexture);
					}
					Writer.WriteArrayEnd();
				}
			}
			if (Settings.bMaxTextureResolution) WriteNumberOrNull(Writer, TEXT("MaxTextureResolution"), Item.MaxTextureResolution);
			if (Settings.bUVChannelCount) WriteNumberOrNull(Writer, TEXT("UVChannels"), Item.UVChannelCount);
			if (Settings.bLightmapResolution) WriteNumberOrNull(Writer, TEXT("LightmapResolution"), Item.LightmapResolution);
			if (Settings.bMaxBoundsLength) WriteNumberOrNull(Writer, TEXT("MaxBoundsLengthM"), Item.MaxBoundsLengthM);
			Writer.WriteObjectEnd();
		}

		/** Returns the JSON report text for the finished meshes and the enabled fields. */
		FString BuildJsonData() const
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			FString JsonText;
			const TSharedRef<FMeshJsonWriter> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			Writer->WriteObjectStart();
			Writer->WriteArrayStart(TEXT("Meshes"));
			for (const FMeshAssetData& Item : AssetItems)
			{
				WriteMeshJson(*Writer, Item, *Settings);
			}
			Writer->WriteArrayEnd();
			Writer->WriteObjectEnd();
			Writer->Close();
			return JsonText;
		}

		/** How long the notification is shown before the first blocking step runs. */
		static constexpr double NotificationDisplayDelaySeconds = 0.2;
		/** Longest time one tick spends processing assets before letting the editor repaint. */
		static constexpr double TickTimeBudgetSeconds = 0.05;
		/** A loaded mesh of the current batch whose render data is still building. */
		struct FMeshWaitingForBuild
		{
			/** Registry entry of the mesh. */
			FAssetData AssetData;
			/** The loaded mesh, read once its build finishes. */
			TWeakObjectPtr<UObject> MeshAsset;
		};

		/** Report rows gathered so far, one per processed mesh. */
		TArray<FMeshAssetData> AssetItems;
		/** Meshes of the current batch still building; the batch is not unloaded until they are gathered. */
		TArray<FMeshWaitingForBuild> MeshesWaitingForBuild;
		/** Meshes found in the configured folders, processed in order. */
		TArray<FAssetData> PendingAssets;
		/** Unloads the packages each batch loaded. */
		Core::FReportPackageUnloader PackageUnloader;
		/** Ticker that runs Tick while a report is in progress; invalid when idle. */
		FTSTicker::FDelegateHandle TickerHandle;
		/** Notification that shows the report progress and the Cancel button. */
		Core::FReportProgressNotification ProgressNotification;
		/** Index in PendingAssets of the next mesh to process. */
		int32 NextAssetIndex = 0;
		/** Stage the next Tick runs. */
		ERunnerStep Step = ERunnerStep::ShowNotification;
		/** When the report started, used to delay blocking work until the notification is visible. */
		double StepStartSeconds = 0.0;
		/** Assets processed in the current batch. */
		int32 AssetsInBatch = 0;
		/** Meshes loaded from disk in the current batch. */
		int32 BatchLoadedAssetCount = 0;
		/** When the current batch started, for the batch timing log. */
		double BatchStartSeconds = 0.0;
		/** Packages that were loaded before the current batch started. */
		TSet<UPackage*> PackagesBeforeBatch;
		/** Name of the last asset processed, shown in the notification. */
		FString CurrentAssetName;
	};

	static TUniquePtr<FMeshAnalyticsRunner> MeshAnalyticsRunner;

	/** Starts the shared Mesh Analytics report runner. */
	void RunMeshAnalyticsReport()
	{
		if (!MeshAnalyticsRunner) MeshAnalyticsRunner = MakeUnique<FMeshAnalyticsRunner>();
		MeshAnalyticsRunner->Start();
	}

	/** Stops the shared report runner and releases it. */
	void StopMeshAnalyticsReport()
	{
		if (MeshAnalyticsRunner)
		{
			MeshAnalyticsRunner->Stop();
			MeshAnalyticsRunner.Reset();
		}
	}
}
}

#undef LOCTEXT_NAMESPACE
