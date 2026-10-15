// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Material/MaterialAnalytics.h"
#include "Core/AssetAnalyticsSettings.h"
#include "Core/AssetAnalyticsUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Engine/Blueprint.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "MaterialAnalytics"

namespace AssetAnalytics
{
namespace Material
{
	/** Kinds of assets that can use a material, following the engine's material usage (vertex factory) families. */
	enum class EMaterialUserType : uint8
	{
		StaticMesh,
		Nanite,
		SkeletalMesh,
		InstancedStaticMesh,
		Niagara,
		Particle,
		GeometryCollection,
		GeometryCache,
		Groom,
		LidarPointCloud,
		Water,
		VolumetricCloud,
		HeterogeneousVolume,
		VirtualHeightfieldMesh,
		Level,
		Blueprint,
		Other,
		Count
	};

	/** CSV column names, in EMaterialUserType order. */
	static const TCHAR* const MaterialUserTypeColumnNames[] =
	{
		TEXT("VF_StaticMesh"),
		TEXT("VF_Nanite"),
		TEXT("VF_SkeletalMesh"),
		TEXT("VF_InstancedStaticMesh"),
		TEXT("VF_Niagara"),
		TEXT("VF_Particle"),
		TEXT("VF_GeometryCollection"),
		TEXT("VF_GeometryCache"),
		TEXT("VF_Groom"),
		TEXT("VF_LidarPointCloud"),
		TEXT("VF_Water"),
		TEXT("VF_VolumetricCloud"),
		TEXT("VF_HeterogeneousVolume"),
		TEXT("VF_VirtualHeightfieldMesh"),
		TEXT("VF_Level"),
		TEXT("VF_Blueprint"),
		TEXT("VF_Other"),
	};
	static_assert(
		UE_ARRAY_COUNT(MaterialUserTypeColumnNames) == static_cast<int32>(EMaterialUserType::Count),
		"Every material user type needs a CSV column name.");

	struct FMaterialAssetData
	{
		/** Creates an empty report row for InAssetData. */
		explicit FMaterialAssetData(const FAssetData& InAssetData)
			: AssetData(InAssetData)
		{
		}

		FAssetData AssetData;
		int64 PackageDiskSize = -1;
		int32 InstanceCount = INDEX_NONE;
		/** Number of distinct assets of each EMaterialUserType that use the material. Empty when not gathered. */
		TArray<int32> UserCounts;
	};

	/**
	 * Decides which kind of placed World Partition actor uses a material, from the actor's class.
	 * Input: ActorAsset is the registry entry of an actor stored under __ExternalActors__.
	 * Returns: A specific actor-based user type, or Level for any other actor.
	 */
	static EMaterialUserType ClassifyPlacedActor(IAssetRegistry& AssetRegistry, const FAssetData& ActorAsset)
	{
		// Plugin classes are compared by path, so their modules are not needed as dependencies.
		static const FTopLevelAssetPath ISMPartitionActorClass(TEXT("/Script/Engine"), TEXT("ISMPartitionActor"));
		static const FTopLevelAssetPath WaterBodyClass(TEXT("/Script/Water"), TEXT("WaterBody"));
		static const FTopLevelAssetPath WaterZoneClass(TEXT("/Script/Water"), TEXT("WaterZone"));
		static const FTopLevelAssetPath VolumetricCloudClass(TEXT("/Script/Engine"), TEXT("VolumetricCloud"));
		static const FTopLevelAssetPath HeterogeneousVolumeClass(TEXT("/Script/Engine"), TEXT("HeterogeneousVolume"));
		static const FTopLevelAssetPath VirtualHeightfieldMeshClass(TEXT("/Script/VirtualHeightfieldMesh"), TEXT("VirtualHeightfieldMesh"));

		// Check the actor class and its parents, so subclasses such as WaterBodyRiver or InstancedFoliageActor match too.
		TArray<FTopLevelAssetPath> ActorClassAndAncestors;
		AssetRegistry.GetAncestorClassNames(ActorAsset.AssetClassPath, ActorClassAndAncestors);
		ActorClassAndAncestors.Add(ActorAsset.AssetClassPath);
		for (const FTopLevelAssetPath& ClassPath : ActorClassAndAncestors)
		{
			if (ClassPath == ISMPartitionActorClass) return EMaterialUserType::InstancedStaticMesh;
			if (ClassPath == WaterBodyClass || ClassPath == WaterZoneClass) return EMaterialUserType::Water;
			if (ClassPath == VolumetricCloudClass) return EMaterialUserType::VolumetricCloud;
			if (ClassPath == HeterogeneousVolumeClass) return EMaterialUserType::HeterogeneousVolume;
			if (ClassPath == VirtualHeightfieldMeshClass) return EMaterialUserType::VirtualHeightfieldMesh;
		}
		return EMaterialUserType::Level;
	}

	/**
	 * Decides which kind of asset a referencing package holds, using only registry data.
	 * Input: ReferencerPackage is a package that references a material or material instance.
	 * Returns: The matching user type, or Other when no rule matches.
	 */
	static EMaterialUserType ClassifyReferencer(IAssetRegistry& AssetRegistry, const FName ReferencerPackage)
	{
		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(ReferencerPackage, PackageAssets);

		// World Partition stores each placed actor in its own package under __ExternalActors__.
		const FString PackageName = ReferencerPackage.ToString();
		if (PackageName.Contains(FPackagePath::GetExternalActorsFolderName()))
		{
			return PackageAssets.Num() > 0
				? ClassifyPlacedActor(AssetRegistry, PackageAssets[0])
				: EMaterialUserType::Level;
		}
		if (PackageAssets.Num() == 0) return EMaterialUserType::Other;

		// Plugin classes are compared by path, so their modules are not needed as dependencies.
		static const FTopLevelAssetPath StaticMeshClass(TEXT("/Script/Engine"), TEXT("StaticMesh"));
		static const FTopLevelAssetPath SkeletalMeshClass(TEXT("/Script/Engine"), TEXT("SkeletalMesh"));
		static const FTopLevelAssetPath NiagaraSystemClass(TEXT("/Script/Niagara"), TEXT("NiagaraSystem"));
		static const FTopLevelAssetPath ParticleSystemClass(TEXT("/Script/Engine"), TEXT("ParticleSystem"));
		static const FTopLevelAssetPath GeometryCollectionClass(TEXT("/Script/GeometryCollectionEngine"), TEXT("GeometryCollection"));
		static const FTopLevelAssetPath GeometryCacheClass(TEXT("/Script/GeometryCache"), TEXT("GeometryCache"));
		static const FTopLevelAssetPath GroomClass(TEXT("/Script/HairStrandsCore"), TEXT("GroomAsset"));
		static const FTopLevelAssetPath LidarPointCloudClass(TEXT("/Script/LidarPointCloudRuntime"), TEXT("LidarPointCloud"));
		static const FTopLevelAssetPath WorldClass(TEXT("/Script/Engine"), TEXT("World"));

		const FAssetData& ReferencerAsset = PackageAssets[0];
		const FTopLevelAssetPath& ClassPath = ReferencerAsset.AssetClassPath;
		if (ClassPath == StaticMeshClass)
		{
			// The NaniteEnabled tag is saved with every static mesh, so Nanite meshes are told apart without loading.
			bool bNaniteEnabled = false;
			ReferencerAsset.GetTagValue(TEXT("NaniteEnabled"), bNaniteEnabled);
			return bNaniteEnabled ? EMaterialUserType::Nanite : EMaterialUserType::StaticMesh;
		}
		if (ClassPath == SkeletalMeshClass) return EMaterialUserType::SkeletalMesh;
		if (ClassPath == NiagaraSystemClass) return EMaterialUserType::Niagara;
		if (ClassPath == ParticleSystemClass) return EMaterialUserType::Particle;
		if (ClassPath == GeometryCollectionClass) return EMaterialUserType::GeometryCollection;
		if (ClassPath == GeometryCacheClass) return EMaterialUserType::GeometryCache;
		if (ClassPath == GroomClass) return EMaterialUserType::Groom;
		if (ClassPath == LidarPointCloudClass) return EMaterialUserType::LidarPointCloud;
		if (ClassPath == WorldClass) return EMaterialUserType::Level;

		// GetClass only finds classes that are already loaded, so no asset is loaded here.
		const UClass* AssetClass = ReferencerAsset.GetClass();
		if (AssetClass != nullptr && AssetClass->IsChildOf(UBlueprint::StaticClass())) return EMaterialUserType::Blueprint;
		return EMaterialUserType::Other;
	}

	/** Returns whether PackageName is inside the project's Developers folder. */
	static bool IsDeveloperPackage(const FName PackageName)
	{
		const FString PackagePath = PackageName.ToString();
		return PackagePath.Equals(TEXT("/Game/Developers"), ESearchCase::IgnoreCase) ||
			PackagePath.StartsWith(TEXT("/Game/Developers/"), ESearchCase::IgnoreCase);
	}

	class FMaterialAnalyticsRunner
	{
	public:
		/** Starts gathering unless a report is already running. */
		void Start()
		{
			if (TickerHandle.IsValid()) return;
			ShowProgressNotification();

			// The folder scan blocks the editor, so give Slate time to draw the notification first.
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::FindAssetsAndStartProcessing),
				NotificationDisplayDelaySeconds);
		}

		/** Stops gathering and throws away unfinished results. */
		void Stop()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			ResetReportData();
			DismissProgressNotification();
		}

	private:
		/** Shows the progress notification used while assets are gathered. */
		void ShowProgressNotification()
		{
			FNotificationInfo NotificationInfo(LOCTEXT("FindingMaterialAssets", "Finding master materials..."));
			NotificationInfo.ButtonDetails.Add(FNotificationButtonInfo(
				LOCTEXT("CancelMaterialAnalytics", "Cancel"),
				LOCTEXT("CancelMaterialAnalyticsTooltip", "Stop gathering material analytics."),
				FSimpleDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::Cancel)));
			NotificationInfo.WidthOverride = 320.0f;
			NotificationInfo.bFireAndForget = false;
			NotificationInfo.FadeInDuration = 0.1f;
			NotificationInfo.FadeOutDuration = 0.5f;
			NotificationInfo.ExpireDuration = 3.0f;
			NotificationInfo.bUseLargeFont = false;
			NotificationInfo.bUseThrobber = false;
			NotificationInfo.bUseSuccessFailIcons = false;
			ProgressNotificationItem = FSlateNotificationManager::Get().AddNotification(NotificationInfo);
			if (ProgressNotificationItem.IsValid()) ProgressNotificationItem->SetCompletionState(SNotificationItem::CS_Pending);
		}

		/** Updates the notification with the current asset count. */
		void UpdateProgressNotification() const
		{
			if (ProgressNotificationItem.IsValid())
			{
				const int32 ProgressPercent = PendingAssets.Num() > 0
					? FMath::RoundToInt(static_cast<float>(NextAssetIndex) / PendingAssets.Num() * 100.0f)
					: 100;
				ProgressNotificationItem->SetText(FText::Format(
					LOCTEXT("MaterialAnalyticsProgress", "Gathering Material Analytics - {0} / {1} assets ({2}%)"),
					FText::AsNumber(NextAssetIndex),
					FText::AsNumber(PendingAssets.Num()),
					FText::AsNumber(ProgressPercent)));
			}
		}

		/** Shows ResultText and State, then closes the progress notification. */
		void FinishProgressNotification(
			const FText& ResultText,
			const SNotificationItem::ECompletionState State)
		{
			if (ProgressNotificationItem.IsValid())
			{
				ProgressNotificationItem->SetText(ResultText);
				ProgressNotificationItem->SetCompletionState(State);
				ProgressNotificationItem->ExpireAndFadeout();
			}
			ProgressNotificationItem.Reset();
		}

		/** Hides the progress notification during shutdown. */
		void DismissProgressNotification()
		{
			if (ProgressNotificationItem.IsValid())
			{
				ProgressNotificationItem->SetCompletionState(SNotificationItem::CS_None);
				ProgressNotificationItem->Fadeout();
			}
			ProgressNotificationItem.Reset();
		}

		/** Clears pending assets, finished rows and the instance tree. */
		void ResetReportData()
		{
			PendingAssets.Reset();
			AssetItems.Reset();
			ChildInstancesByParent.Reset();
			InstancePackages.Reset();
			UserTypeByPackage.Reset();
			NextAssetIndex = 0;
		}

		/** Cancels gathering, clears unfinished results, and updates the notification. */
		void Cancel()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			ResetReportData();
			FinishProgressNotification(
				LOCTEXT("MaterialAnalyticsCancelled", "Material Analytics gathering cancelled."),
				SNotificationItem::CS_None);
		}

		/**
		 * Maps every material instance in the project to its parent, without loading anything.
		 * Instances often live outside the search folders, so the whole registry is used.
		 */
		void BuildInstanceTree(IAssetRegistry& AssetRegistry)
		{
			// Instances outside the scanned folders are only known once the startup scan has finished.
			AssetRegistry.WaitForCompletion();

			TArray<FAssetData> MaterialInstances;
			AssetRegistry.GetAssetsByClass(
				UMaterialInstanceConstant::StaticClass()->GetClassPathName(),
				MaterialInstances,
				true);
			for (const FAssetData& MaterialInstance : MaterialInstances)
			{
				FString ParentExportTextPath;
				if (!MaterialInstance.GetTagValue(TEXT("Parent"), ParentExportTextPath)) continue;

				const FSoftObjectPath ParentPath(FPackageName::ExportTextPathToObjectPath(ParentExportTextPath));
				if (ParentPath.IsNull()) continue;
				ChildInstancesByParent.FindOrAdd(ParentPath).Add(MaterialInstance);
				InstancePackages.Add(MaterialInstance.PackageName);
			}
		}

		/**
		 * Finds the instances below MaterialPath, including instances of instances.
		 * Input: Settings decides whether unreferenced or Developers-only instances are skipped.
		 * Returns: The counted instances. A skipped instance has no counted children,
		 * because any child would itself be a reference to it.
		 */
		TArray<FAssetData> GatherCountedInstances(
			IAssetRegistry& AssetRegistry,
			const FSoftObjectPath& MaterialPath,
			const UAssetAnalyticsSettings& Settings) const
		{
			TSet<FSoftObjectPath> VisitedInstances;
			TArray<FSoftObjectPath> PathsToVisit;
			PathsToVisit.Add(MaterialPath);
			TArray<FAssetData> CountedInstances;
			while (PathsToVisit.Num() > 0)
			{
				const FSoftObjectPath CurrentPath = PathsToVisit.Pop(EAllowShrinking::No);
				const TArray<FAssetData>* ChildInstances = ChildInstancesByParent.Find(CurrentPath);
				if (ChildInstances == nullptr) continue;

				for (const FAssetData& ChildInstance : *ChildInstances)
				{
					// The visited check guards against broken data that loops back on itself.
					bool bAlreadyVisited = false;
					VisitedInstances.Add(ChildInstance.GetSoftObjectPath(), &bAlreadyVisited);
					if (bAlreadyVisited) continue;
					if (Settings.bIgnoreUnreferencedAssets &&
						!Core::HasRelevantReferencer(AssetRegistry, ChildInstance, Settings.bIgnoreDeveloperReferences)) continue;

					CountedInstances.Add(ChildInstance);
					PathsToVisit.Add(ChildInstance.GetSoftObjectPath());
				}
			}
			return CountedInstances;
		}

		/**
		 * Counts the distinct assets of each user type that reference the master or its counted instances.
		 * Material instances are skipped because the instance count already covers them.
		 * Output: Fills Item.UserCounts, one entry per EMaterialUserType.
		 */
		void CountUsersByType(
			IAssetRegistry& AssetRegistry,
			const TArray<FAssetData>& MaterialAndInstances,
			const UAssetAnalyticsSettings& Settings,
			FMaterialAssetData& Item)
		{
			TSet<FName> UserPackages;
			for (const FAssetData& MaterialOrInstance : MaterialAndInstances)
			{
				TArray<FName> Referencers;
				AssetRegistry.GetReferencers(
					MaterialOrInstance.PackageName,
					Referencers,
					UE::AssetRegistry::EDependencyCategory::Package,
					UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Game));
				for (const FName Referencer : Referencers)
				{
					if (InstancePackages.Contains(Referencer)) continue;
					if (Settings.bIgnoreDeveloperReferences && IsDeveloperPackage(Referencer)) continue;
					UserPackages.Add(Referencer);
				}
			}

			Item.UserCounts.Init(0, static_cast<int32>(EMaterialUserType::Count));
			for (const FName UserPackage : UserPackages)
			{
				const EMaterialUserType* CachedUserType = UserTypeByPackage.Find(UserPackage);
				const EMaterialUserType UserType = CachedUserType != nullptr
					? *CachedUserType
					: UserTypeByPackage.Add(UserPackage, ClassifyReferencer(AssetRegistry, UserPackage));
				++Item.UserCounts[static_cast<int32>(UserType)];
			}
		}

		/** Finds the master materials in the configured folders and builds the instance tree when needed. */
		void BeginGatheringAssets()
		{
			ResetReportData();

			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			FARFilter Filter;
			// Subclasses are left out so material instances are not reported as masters.
			Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = true;
			for (const FDirectoryPath& Directory : Settings->MaterialSearchFolders)
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
						TEXT("Asset Analytics skipped material search folder '%s' because it is invalid or its content root is not mounted."),
						*Directory.Path);
				}
			}
			if (Filter.PackagePaths.Num() == 0) return;

			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			TArray<FString> PathsToScan;
			for (const FName PackagePath : Filter.PackagePaths)
			{
				PathsToScan.Add(PackagePath.ToString());
			}
			AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
			AssetRegistry.GetAssets(Filter, PendingAssets);

			const bool bNeedsInstanceTree = Settings->bMaterialInstanceCounts || Settings->bMaterialUserTypeCounts;
			if (bNeedsInstanceTree && PendingAssets.Num() > 0) BuildInstanceTree(AssetRegistry);
		}

		/**
		 * Finds the master materials, then hands over to Tick for batch processing.
		 * Returns: False, so this one-shot ticker is removed.
		 */
		bool FindAssetsAndStartProcessing(float)
		{
			TickerHandle.Reset();
			BeginGatheringAssets();
			UpdateProgressNotification();
			if (PendingAssets.Num() == 0)
			{
				CompleteReport();
				return false;
			}
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::Tick));
			return false;
		}

		/**
		 * Fills report rows for the next batch of master materials from registry data.
		 * Returns: True while more assets are waiting.
		 */
		bool Tick(float)
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			const int32 BatchEnd = FMath::Min(NextAssetIndex + FMath::Max(Settings->AssetsPerTick, 1), PendingAssets.Num());

			for (; NextAssetIndex < BatchEnd; ++NextAssetIndex)
			{
				const FAssetData& AssetData = PendingAssets[NextAssetIndex];
				if (Settings->bIgnoreUnreferencedAssets &&
					!Core::HasRelevantReferencer(AssetRegistry, AssetData, Settings->bIgnoreDeveloperReferences)) continue;

				FMaterialAssetData& Item = AssetItems.Emplace_GetRef(AssetData);
				if (Settings->bMaterialPackageDiskSize)
				{
					FAssetPackageData PackageData;
					if (AssetRegistry.TryGetAssetPackageData(AssetData.PackageName, PackageData) ==
						UE::AssetRegistry::EExists::Exists)
					{
						Item.PackageDiskSize = PackageData.DiskSize;
					}
				}
				if (Settings->bMaterialInstanceCounts || Settings->bMaterialUserTypeCounts)
				{
					TArray<FAssetData> MaterialAndInstances =
						GatherCountedInstances(AssetRegistry, AssetData.GetSoftObjectPath(), *Settings);
					Item.InstanceCount = MaterialAndInstances.Num();
					if (Settings->bMaterialUserTypeCounts)
					{
						MaterialAndInstances.Add(AssetData);
						CountUsersByType(AssetRegistry, MaterialAndInstances, *Settings, Item);
					}
				}
			}
			UpdateProgressNotification();

			if (NextAssetIndex >= PendingAssets.Num())
			{
				TickerHandle.Reset();
				CompleteReport();
				return false;
			}
			return true;
		}

		/** Sorts the results and saves the CSV. The browser report is added in a later step. */
		void CompleteReport()
		{
			AssetItems.Sort([](const FMaterialAssetData& Left, const FMaterialAssetData& Right)
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
			const FString CsvPath = FPaths::Combine(ReportDirectory, TEXT("MaterialAnalytics.csv"));
			if (!FFileHelper::SaveStringToFile(
				BuildCsvData(),
				*CsvPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				FinishProgressNotification(
					LOCTEXT("MaterialCsvSaveFailed", "The material analytics CSV file could not be saved."),
					SNotificationItem::CS_Fail);
				ResetReportData();
				return;
			}

			UE_LOG(LogTemp, Log, TEXT("Asset Analytics saved the Material Analytics CSV to %s"), *FPaths::ConvertRelativePathToFull(CsvPath));
			FinishProgressNotification(
				FText::Format(
					LOCTEXT("MaterialAnalyticsComplete", "Material Analytics CSV saved for {0} master materials."),
					FText::AsNumber(AssetItems.Num())),
				SNotificationItem::CS_Success);
			ResetReportData();
		}

		/** Returns CSV text containing the enabled columns and finished rows. */
		FString BuildCsvData() const
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			TArray<FString> HeaderFields;
			HeaderFields.Add(TEXT("AssetName"));
			HeaderFields.Add(TEXT("PackagePath"));
			if (Settings->bMaterialPackageDiskSize) HeaderFields.Add(TEXT("PackageDiskSize"));
			if (Settings->bMaterialInstanceCounts) HeaderFields.Add(TEXT("Instances"));
			if (Settings->bMaterialUserTypeCounts)
			{
				for (const TCHAR* ColumnName : MaterialUserTypeColumnNames)
				{
					HeaderFields.Add(ColumnName);
				}
			}

			FString CsvData = FString::Join(HeaderFields, TEXT(",")) + TEXT("\r\n");
			for (const FMaterialAssetData& Item : AssetItems)
			{
				TArray<FString> RowFields;
				RowFields.Add(Core::EscapeCsvField(Item.AssetData.AssetName.ToString()));
				RowFields.Add(Core::EscapeCsvField(Item.AssetData.PackagePath.ToString()));
				if (Settings->bMaterialPackageDiskSize) RowFields.Add(Core::CsvNumberOrEmpty(Item.PackageDiskSize));
				if (Settings->bMaterialInstanceCounts) RowFields.Add(Core::CsvNumberOrEmpty(Item.InstanceCount));
				if (Settings->bMaterialUserTypeCounts)
				{
					for (const int32 UserCount : Item.UserCounts)
					{
						RowFields.Add(Core::CsvNumberOrEmpty(UserCount));
					}
				}
				CsvData += FString::Join(RowFields, TEXT(",")) + TEXT("\r\n");
			}
			return CsvData;
		}

		static constexpr float NotificationDisplayDelaySeconds = 0.2f;
		TArray<FMaterialAssetData> AssetItems;
		TArray<FAssetData> PendingAssets;
		/** Direct child instances for every material or instance that has any, keyed by the parent's object path. */
		TMap<FSoftObjectPath, TArray<FAssetData>> ChildInstancesByParent;
		/** Packages of every material instance in the instance tree, so they are not counted as users. */
		TSet<FName> InstancePackages;
		/** User type of each referencing package classified so far in this run. */
		TMap<FName, EMaterialUserType> UserTypeByPackage;
		FTSTicker::FDelegateHandle TickerHandle;
		TSharedPtr<SNotificationItem> ProgressNotificationItem;
		int32 NextAssetIndex = 0;
	};

	static TUniquePtr<FMaterialAnalyticsRunner> MaterialAnalyticsRunner;

	/** Starts the shared Material Analytics report runner. */
	void RunMaterialAnalyticsReport()
	{
		if (!MaterialAnalyticsRunner) MaterialAnalyticsRunner = MakeUnique<FMaterialAnalyticsRunner>();
		MaterialAnalyticsRunner->Start();
	}

	/** Stops the shared report runner and releases it. */
	void StopMaterialAnalyticsReport()
	{
		if (MaterialAnalyticsRunner)
		{
			MaterialAnalyticsRunner->Stop();
			MaterialAnalyticsRunner.Reset();
		}
	}
}
}

#undef LOCTEXT_NAMESPACE
