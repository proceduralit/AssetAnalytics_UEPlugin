// Copyright (c) 2026 Mohsen Tabasi
// SPDX-License-Identifier: MIT

#include "Material/MaterialAnalytics.h"
#include "Core/AssetAnalyticsPackageUnloader.h"
#include "Core/AssetAnalyticsProgressNotification.h"
#include "Core/AssetAnalyticsReportBridge.h"
#include "Core/AssetAnalyticsSettings.h"
#include "Core/AssetAnalyticsUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Particles/Material/ParticleModuleMeshMaterial.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSystem.h"
#include "Particles/TypeData/ParticleModuleTypeDataAnimTrail.h"
#include "Particles/TypeData/ParticleModuleTypeDataBeam2.h"
#include "Particles/TypeData/ParticleModuleTypeDataMesh.h"
#include "Particles/TypeData/ParticleModuleTypeDataRibbon.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialCachedData.h"
#include "MaterialShaderQualitySettings.h"
#include "MaterialShared.h"
#include "RHIShaderPlatform.h"
#include "SceneTypes.h"
#include "ShaderPlatformQualitySettings.h"
#include "StaticParameterSet.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SoftObjectPath.h"

#define LOCTEXT_NAMESPACE "MaterialAnalytics"

namespace AssetAnalytics
{
namespace Material
{
	/** Kinds of material users that can be recognised from Asset Registry data alone. */
	enum class EMaterialUserType : uint8
	{
		StaticMesh,
		Nanite,
		SkeletalMesh,
		InstancedStaticMeshes,
		Niagara,
		Particle,
		GeometryCollections,
		GeometryCache,
		HairStrands,
		LidarPointCloud,
		Water,
		VolumetricCloud,
		HeterogeneousVolumes,
		VirtualHeightfieldMesh,
		Level,
		Blueprint,
		Other,
		/** Also used in the vertex factory table for entries whose users cannot be found without loading. */
		Count
	};

	/** One entry of the VertexFactories object in the JSON report. */
	struct FVertexFactoryEntry
	{
		/** JSON key: the engine's EMaterialUsage name without MATUSAGE_, or a user type that has no usage flag. */
		const TCHAR* Name;

		/** Registry user type listed under this entry, or Count when its users are only known after loading. */
		EMaterialUserType RegistryUserType;

		/** Engine usage flag this entry reports as Enabled, or MATUSAGE_MAX for entries that are not usage flags. */
		EMaterialUsage Usage;
	};

	/**
	 * Every entry written under VertexFactories: all EMaterialUsage flags in engine order,
	 * then the user types that do not match a single usage flag.
	 */
	static const FVertexFactoryEntry VertexFactoryEntries[] =
	{
		{ TEXT("SkeletalMesh"), EMaterialUserType::SkeletalMesh, MATUSAGE_SkeletalMesh },
		{ TEXT("ParticleSprites"), EMaterialUserType::Count, MATUSAGE_ParticleSprites },
		{ TEXT("BeamTrails"), EMaterialUserType::Count, MATUSAGE_BeamTrails },
		{ TEXT("MeshParticles"), EMaterialUserType::Count, MATUSAGE_MeshParticles },
		{ TEXT("StaticLighting"), EMaterialUserType::Count, MATUSAGE_StaticLighting },
		{ TEXT("MorphTargets"), EMaterialUserType::Count, MATUSAGE_MorphTargets },
		{ TEXT("SplineMesh"), EMaterialUserType::Count, MATUSAGE_SplineMesh },
		{ TEXT("InstancedStaticMeshes"), EMaterialUserType::InstancedStaticMeshes, MATUSAGE_InstancedStaticMeshes },
		{ TEXT("GeometryCollections"), EMaterialUserType::GeometryCollections, MATUSAGE_GeometryCollections },
		{ TEXT("Clothing"), EMaterialUserType::Count, MATUSAGE_Clothing },
		{ TEXT("NiagaraSprites"), EMaterialUserType::Count, MATUSAGE_NiagaraSprites },
		{ TEXT("NiagaraRibbons"), EMaterialUserType::Count, MATUSAGE_NiagaraRibbons },
		{ TEXT("NiagaraMeshParticles"), EMaterialUserType::Count, MATUSAGE_NiagaraMeshParticles },
		{ TEXT("GeometryCache"), EMaterialUserType::GeometryCache, MATUSAGE_GeometryCache },
		{ TEXT("Water"), EMaterialUserType::Water, MATUSAGE_Water },
		{ TEXT("HairStrands"), EMaterialUserType::HairStrands, MATUSAGE_HairStrands },
		{ TEXT("LidarPointCloud"), EMaterialUserType::LidarPointCloud, MATUSAGE_LidarPointCloud },
		{ TEXT("VirtualHeightfieldMesh"), EMaterialUserType::VirtualHeightfieldMesh, MATUSAGE_VirtualHeightfieldMesh },
		{ TEXT("Nanite"), EMaterialUserType::Nanite, MATUSAGE_Nanite },
		{ TEXT("Voxels"), EMaterialUserType::Count, MATUSAGE_Voxels },
		{ TEXT("VolumetricCloud"), EMaterialUserType::VolumetricCloud, MATUSAGE_VolumetricCloud },
		{ TEXT("HeterogeneousVolumes"), EMaterialUserType::HeterogeneousVolumes, MATUSAGE_HeterogeneousVolumes },
		{ TEXT("StaticMesh"), EMaterialUserType::StaticMesh, MATUSAGE_StaticMesh },
		{ TEXT("EditorCompositing"), EMaterialUserType::Count, MATUSAGE_EditorCompositing },
		{ TEXT("NeuralNetworks"), EMaterialUserType::Count, MATUSAGE_NeuralNetworks },
		{ TEXT("MeshDeformer"), EMaterialUserType::Count, MATUSAGE_MeshDeformer },
		{ TEXT("Niagara"), EMaterialUserType::Niagara, MATUSAGE_MAX },
		{ TEXT("Particle"), EMaterialUserType::Particle, MATUSAGE_MAX },
		{ TEXT("Level"), EMaterialUserType::Level, MATUSAGE_MAX },
		{ TEXT("Blueprint"), EMaterialUserType::Blueprint, MATUSAGE_MAX },
		{ TEXT("Other"), EMaterialUserType::Other, MATUSAGE_MAX },
	};

	/** A classified asset that references a material or one of its instances. */
	struct FMaterialUser
	{
		EMaterialUserType Type = EMaterialUserType::Other;

		/** Object path shown in the report, or the package name when the package has no registry asset. */
		FString Path;
	};

	/** One static switch of a master and how often its counted instances change it. */
	struct FStaticSwitchSummary
	{
		/** Identifies the switch, including its layer when it comes from a material layer. */
		FMaterialParameterInfo ParameterInfo;

		/** Value the master uses when an instance does not override the switch. */
		bool bDefaultValue = false;

		/** Counted instances that override the switch with a value different from the default. */
		int32 ChangedCount = 0;
	};

	struct FMaterialAssetData
	{
		/** Creates an empty report row for InAssetData. */
		explicit FMaterialAssetData(const FAssetData& InAssetData)
			: AssetData(InAssetData)
		{
		}

		FAssetData AssetData;
		int64 PackageDiskSize = -1;

		/** Counted instances below the material, including instances of instances. */
		TArray<FAssetData> Instances;

		/** User paths for each EMaterialUserType, sorted. Empty when users were not gathered. */
		TArray<TArray<FString>> UserPathsByType;

		/** Whether each EMaterialUsage flag is enabled on the master. Empty when the master was not read. */
		TArray<bool> UsageEnabled;

		/** Loaded users found for each EMaterialUsage flag, sorted. Empty when users were not loaded. */
		TArray<TArray<FString>> ResolvedUserPathsByUsage;

		/** Whether the counted instances were loaded to find which ones compile their own shader map. */
		bool bShaderPermutationsRead = false;

		/** Counted instances that compile their own shader map, sorted. */
		TArray<FString> StaticPermutationInstancePaths;

		/** Quality levels the master's Quality Switch nodes give their own shader map. */
		TArray<FString> QualityLevelsUsed;

		/** Shader maps of the master and its static permutation instances, including quality levels. */
		int32 ShaderMapCount = INDEX_NONE;

		/** Shader maps of the master and one instance per unique static permutation, including quality levels. */
		int32 UniqueShaderMapCount = INDEX_NONE;

		/** Shaders the master and one instance per unique static permutation compile, from the engine's shader map layout. */
		int32 LayoutShaderCount = INDEX_NONE;

		/** LayoutShaderCount split by vertex factory name, or "None" for shaders without one. */
		TMap<FString, int32> LayoutShadersByVertexFactory;

		/** Static switches of the master, with how many counted instances change them. */
		TArray<FStaticSwitchSummary> StaticSwitches;

		/** Static permutation instances whose chain overrides static switches, layers or base properties, or none of them. */
		int32 SwitchPermutationCount = 0;
		int32 LayerPermutationCount = 0;
		int32 BasePropertyPermutationCount = 0;
		int32 OtherPermutationCount = 0;

		/** Distinct combinations of static parameters and base property overrides among the static permutation instances. */
		int32 UniqueStaticPermutationCount = 0;
	};

	/** What an instance and its parent instances override, which explains why it compiles its own shader map. */
	struct FStaticPermutationReasons
	{
		bool bStaticSwitches = false;
		bool bMaterialLayers = false;
		bool bBaseProperties = false;
	};

	/**
	 * Collects the overrides of Instance and its parent instances, because a child inherits its parents'
	 * static overrides and so compiles its own shader map even when it sets nothing itself.
	 */
	static FStaticPermutationReasons FindStaticPermutationReasons(const UMaterialInstance& Instance)
	{
		FStaticPermutationReasons Reasons;
		Reasons.bBaseProperties = Instance.HasOverridenBaseProperties();
		for (const UMaterialInstance* ChainInstance = &Instance;
			ChainInstance != nullptr;
			ChainInstance = Cast<UMaterialInstance>(ChainInstance->Parent))
		{
			const FStaticParameterSet OwnStaticParameters = ChainInstance->GetStaticParameters();
			if (OwnStaticParameters.bHasMaterialLayers) Reasons.bMaterialLayers = true;
			for (const FStaticSwitchParameter& SwitchParameter : OwnStaticParameters.StaticSwitchParameters)
			{
				if (SwitchParameter.bOverride) Reasons.bStaticSwitches = true;
			}
		}
		return Reasons;
	}

	/** Effective static parameters and base property overrides of one static permutation instance, used to group equal ones. */
	struct FStaticPermutationKey
	{
		FStaticParameterSet StaticParameters;
		uint64 BasePropertyOverridesHash = 0;
	};

	/** Quality levels that compile their own shader map for one material, and the shader map count they produce. */
	struct FQualityShaderMaps
	{
		/** Quality levels whose Quality Switch inputs differ, or that a platform quality override forces. */
		TArray<EMaterialQualityLevel::Type> UsedLevels;

		/** One shader map per used level, plus one default shader map for the levels that are not used. */
		int32 ShaderMapCount = 1;
	};

	/**
	 * Finds how many shader maps Material compiles across quality levels on ShaderPlatform when cooking.
	 * It repeats UMaterialInterface::GetQualityLevelUsage (not exported from the Engine module) and
	 * UMaterial::GetNewResources: each used quality level gets its own shader map, and the unused levels share a
	 * default one unless the platform discards them when cooking.
	 */
	static FQualityShaderMaps GetQualityShaderMaps(const UMaterialInterface& Material, const EShaderPlatform ShaderPlatform)
	{
		// Quality levels whose Quality Switch inputs differ, including switches inside material functions.
		const TArray<bool>& SwitchQualityLevelsUsed = Material.GetCachedExpressionData().QualityLevelsUsed;
		const UShaderPlatformQualitySettings* QualitySettings =
			UMaterialShaderQualitySettings::Get()->GetShaderPlatformQualitySettings(ShaderPlatform);

		FQualityShaderMaps Result;
		Result.ShaderMapCount = 0;
		bool bNeedsDefaultShaderMap = false;
		for (int32 QualityIndex = 0; QualityIndex < EMaterialQualityLevel::Num; ++QualityIndex)
		{
			const EMaterialQualityLevel::Type QualityLevel = static_cast<EMaterialQualityLevel::Type>(QualityIndex);
			bool bQualityLevelUsed = SwitchQualityLevelsUsed.IsValidIndex(QualityIndex) && SwitchQualityLevelsUsed[QualityIndex];
			bool bDiscardedWhenCooking = false;
			if (QualitySettings != nullptr)
			{
				const FMaterialQualityOverrides& QualityOverrides = QualitySettings->GetQualityOverrides(QualityLevel);
				if (QualityOverrides.bDiscardQualityDuringCook)
				{
					bQualityLevelUsed = false;
					bDiscardedWhenCooking = true;
				}
				else if (QualityOverrides.bEnableOverride &&
					QualityOverrides.HasAnyOverridesSet() &&
					QualityOverrides.CanOverride(ShaderPlatform))
				{
					// Platform quality overrides change the shader, so this level compiles separately too.
					bQualityLevelUsed = true;
				}
			}

			if (bQualityLevelUsed)
			{
				Result.UsedLevels.Add(QualityLevel);
				++Result.ShaderMapCount;
			}
			else if (!bDiscardedWhenCooking)
			{
				bNeedsDefaultShaderMap = true;
			}
		}
		if (bNeedsDefaultShaderMap) ++Result.ShaderMapCount;
		return Result;
	}

	/** Breakdown key for material shaders that do not belong to a vertex factory. */
	static const TCHAR* NoVertexFactoryName = TEXT("None");

	/**
	 * Counts the shaders Material's shader maps compile on ShaderPlatform, from the engine's shader map layout.
	 * Nothing is compiled: the layout lists every shader type each vertex factory would cache, using conservative
	 * material parameters, so the count can be slightly higher than a cook. This is the count the material editor's
	 * shader stats and the DumpMaterialShaderTypes commandlet use.
	 * A master covers all its quality shader maps; an instance only covers its active quality level.
	 * Inputs: CookedPlatform selects the cooked game's shader permutations (nullptr uses the editor's, which adds
	 * editor-only shaders). Multiplier repeats the count, for instance quality shader maps the layout does not cover.
	 * Output: adds each vertex factory's shaders to InOutShadersByVertexFactory and returns the shader count.
	 */
	static int32 CountLayoutShaders(
		UMaterialInterface& Material,
		const EShaderPlatform ShaderPlatform,
		const ITargetPlatform* CookedPlatform,
		const int32 Multiplier,
		TMap<FString, int32>& InOutShadersByVertexFactory)
	{
		TArray<FDebugShaderTypeInfo> ShaderInfos;
		Material.GetShaderTypes(ShaderPlatform, CookedPlatform, ShaderInfos);

		int32 ShaderCount = 0;
		for (const FDebugShaderTypeInfo& ShaderInfo : ShaderInfos)
		{
			int32 VertexFactoryShaderCount = ShaderInfo.ShaderTypes.Num();
			for (const FDebugShaderPipelineInfo& PipelineInfo : ShaderInfo.Pipelines)
			{
				VertexFactoryShaderCount += PipelineInfo.ShaderTypes.Num();
			}
			const FString VertexFactoryName = ShaderInfo.VFType != nullptr ? ShaderInfo.VFType->GetName() : NoVertexFactoryName;
			InOutShadersByVertexFactory.FindOrAdd(VertexFactoryName) += VertexFactoryShaderCount * Multiplier;
			ShaderCount += VertexFactoryShaderCount;
		}
		return ShaderCount * Multiplier;
	}

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
			if (ClassPath == ISMPartitionActorClass) return EMaterialUserType::InstancedStaticMeshes;
			if (ClassPath == WaterBodyClass || ClassPath == WaterZoneClass) return EMaterialUserType::Water;
			if (ClassPath == VolumetricCloudClass) return EMaterialUserType::VolumetricCloud;
			if (ClassPath == HeterogeneousVolumeClass) return EMaterialUserType::HeterogeneousVolumes;
			if (ClassPath == VirtualHeightfieldMeshClass) return EMaterialUserType::VirtualHeightfieldMesh;
		}
		return EMaterialUserType::Level;
	}

	/**
	 * Decides which kind of asset a referencing package holds, using only registry data.
	 * Input: ReferencerPackage is a package that references a material or material instance.
	 * Returns: The matching user type, or Other when no rule matches.
	 */
	static EMaterialUserType ClassifyReferencer(
		IAssetRegistry& AssetRegistry,
		const FName ReferencerPackage,
		const TArray<FAssetData>& PackageAssets)
	{
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
		if (ClassPath == GeometryCollectionClass) return EMaterialUserType::GeometryCollections;
		if (ClassPath == GeometryCacheClass) return EMaterialUserType::GeometryCache;
		if (ClassPath == GroomClass) return EMaterialUserType::HairStrands;
		if (ClassPath == LidarPointCloudClass) return EMaterialUserType::LidarPointCloud;
		if (ClassPath == WorldClass) return EMaterialUserType::Level;

		// GetClass only finds classes that are already loaded, so no asset is loaded here.
		const UClass* AssetClass = ReferencerAsset.GetClass();
		if (AssetClass != nullptr && AssetClass->IsChildOf(UBlueprint::StaticClass())) return EMaterialUserType::Blueprint;
		return EMaterialUserType::Other;
	}

	/**
	 * Classifies a referencing package and picks the path shown for it in the report.
	 * Returns: The user's type and its object path, or its package name when the registry has no asset for it.
	 */
	static FMaterialUser MakeMaterialUser(IAssetRegistry& AssetRegistry, const FName ReferencerPackage)
	{
		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(ReferencerPackage, PackageAssets);

		FMaterialUser User;
		User.Type = ClassifyReferencer(AssetRegistry, ReferencerPackage, PackageAssets);
		User.Path = PackageAssets.Num() > 0
			? PackageAssets[0].GetObjectPathString()
			: ReferencerPackage.ToString();
		return User;
	}

	/** Returns the object path of InstanceData's parent material, read from its registry tag. */
	static FString GetParentPath(const FAssetData& InstanceData)
	{
		FString ParentExportTextPath;
		InstanceData.GetTagValue(TEXT("Parent"), ParentExportTextPath);
		return FPackageName::ExportTextPathToObjectPath(ParentExportTextPath);
	}

	/** One way a loaded user draws materials: the usage flag it needs and the materials it draws that way. */
	struct FMaterialUse
	{
		/** Usage flag needed, or MATUSAGE_MAX when the component or emitter type is not recognised. */
		EMaterialUsage Usage = MATUSAGE_MAX;

		/** Materials drawn this way. Empty means the use applies to every material of the user. */
		TArray<FSoftObjectPath> Materials;
	};

	/** Adds a use of Usage for the valid entries of Materials to Uses. */
	static void AddMaterialUse(
		TArray<FMaterialUse>& Uses,
		const EMaterialUsage Usage,
		const TArray<UMaterialInterface*>& Materials)
	{
		FMaterialUse& Use = Uses.AddDefaulted_GetRef();
		Use.Usage = Usage;
		for (const UMaterialInterface* Material : Materials)
		{
			if (Material != nullptr) Use.Materials.AddUnique(FSoftObjectPath(Material));
		}
	}

	/**
	 * Returns the usage flag a component needs for the materials it draws.
	 * Returns: MATUSAGE_MAX for component types this report does not recognise.
	 */
	static EMaterialUsage GetComponentUsage(const UPrimitiveComponent& Component)
	{
		// Subclasses are checked before UStaticMeshComponent because they derive from it.
		if (Component.IsA<USplineMeshComponent>()) return MATUSAGE_SplineMesh;
		if (Component.IsA<UInstancedStaticMeshComponent>()) return MATUSAGE_InstancedStaticMeshes;
		if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(&Component))
		{
			const UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
			return StaticMesh != nullptr && StaticMesh->IsNaniteEnabled() ? MATUSAGE_Nanite : MATUSAGE_StaticMesh;
		}
		if (Component.IsA<USkeletalMeshComponent>()) return MATUSAGE_SkeletalMesh;

		// Compared by path, so the GeometryCollectionEngine module is not needed as a dependency.
		static const FTopLevelAssetPath GeometryCollectionComponentClass(
			TEXT("/Script/GeometryCollectionEngine"), TEXT("GeometryCollectionComponent"));
		for (const UClass* Class = Component.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
		{
			if (Class->GetClassPathName() == GeometryCollectionComponentClass) return MATUSAGE_GeometryCollections;
		}
		return MATUSAGE_MAX;
	}

	/**
	 * Lists the materials drawn by every primitive component of a Blueprint, including inherited ones.
	 * Input: Blueprint must be loaded. Components overridden in child Blueprints are read from their parent.
	 * Returns: One use per component.
	 */
	static TArray<FMaterialUse> FindBlueprintUses(const UBlueprint& Blueprint)
	{
		TArray<const UActorComponent*> ComponentTemplates;
		for (UClass* Class = Blueprint.GeneratedClass; Class != nullptr; Class = Class->GetSuperClass())
		{
			const UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(Class);
			if (BlueprintClass == nullptr || BlueprintClass->SimpleConstructionScript == nullptr) continue;
			for (const USCS_Node* Node : BlueprintClass->SimpleConstructionScript->GetAllNodes())
			{
				if (Node != nullptr && Node->ComponentTemplate != nullptr) ComponentTemplates.Add(Node->ComponentTemplate);
			}
		}

		// Components created in native C++ constructors live on the class default object instead.
		if (Blueprint.GeneratedClass != nullptr)
		{
			if (const AActor* DefaultActor = Cast<AActor>(Blueprint.GeneratedClass->GetDefaultObject()))
			{
				for (const UActorComponent* Component : DefaultActor->GetComponents())
				{
					if (Component != nullptr) ComponentTemplates.Add(Component);
				}
			}
		}

		TArray<FMaterialUse> Uses;
		for (const UActorComponent* ComponentTemplate : ComponentTemplates)
		{
			const UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(ComponentTemplate);
			if (PrimitiveComponent == nullptr) continue;

			TArray<UMaterialInterface*> UsedMaterials;
			PrimitiveComponent->GetUsedMaterials(UsedMaterials);
			AddMaterialUse(Uses, GetComponentUsage(*PrimitiveComponent), UsedMaterials);
		}
		return Uses;
	}

	/**
	 * Lists the materials drawn by each emitter of a Cascade particle system, using its highest-detail LOD.
	 * Returns: One use per emitter, with the usage flag its emitter type needs.
	 */
	static TArray<FMaterialUse> FindParticleSystemUses(const UParticleSystem& ParticleSystem)
	{
		TArray<FMaterialUse> Uses;
		for (const UParticleEmitter* Emitter : ParticleSystem.Emitters)
		{
			if (Emitter == nullptr || Emitter->LODLevels.Num() == 0 || Emitter->LODLevels[0] == nullptr) continue;
			const UParticleLODLevel& LODLevel = *Emitter->LODLevels[0];

			TArray<UMaterialInterface*> EmitterMaterials;
			if (LODLevel.RequiredModule != nullptr) EmitterMaterials.Add(LODLevel.RequiredModule->Material);
			for (const UParticleModule* Module : LODLevel.Modules)
			{
				if (const UParticleModuleMeshMaterial* MeshMaterialModule = Cast<UParticleModuleMeshMaterial>(Module))
				{
					EmitterMaterials.Append(MeshMaterialModule->MeshMaterials);
				}
			}

			// Emitters without a type data module draw sprites.
			EMaterialUsage Usage = MATUSAGE_ParticleSprites;
			const UParticleModuleTypeDataBase* TypeData = LODLevel.TypeDataModule;
			if (const UParticleModuleTypeDataMesh* MeshTypeData = Cast<UParticleModuleTypeDataMesh>(TypeData))
			{
				Usage = MATUSAGE_MeshParticles;
				if (MeshTypeData->Mesh != nullptr)
				{
					for (const FStaticMaterial& StaticMaterial : MeshTypeData->Mesh->GetStaticMaterials())
					{
						EmitterMaterials.Add(StaticMaterial.MaterialInterface);
					}
				}
			}
			else if (TypeData != nullptr &&
				(TypeData->IsA<UParticleModuleTypeDataBeam2>() ||
				TypeData->IsA<UParticleModuleTypeDataRibbon>() ||
				TypeData->IsA<UParticleModuleTypeDataAnimTrail>()))
			{
				Usage = MATUSAGE_BeamTrails;
			}
			AddMaterialUse(Uses, Usage, EmitterMaterials);
		}
		return Uses;
	}

	/**
	 * Lists the skeletal mesh features that need their own usage flags. They apply to every material of the
	 * mesh, because the report does not check which sections have morph targets or cloth.
	 * Returns: One use per feature found.
	 */
	static TArray<FMaterialUse> FindSkeletalMeshUses(USkeletalMesh& SkeletalMesh)
	{
		TArray<FMaterialUse> Uses;
		if (SkeletalMesh.GetMorphTargets().Num() > 0) Uses.Add({ MATUSAGE_MorphTargets, {} });
		if (SkeletalMesh.HasActiveClothingAssets()) Uses.Add({ MATUSAGE_Clothing, {} });
		if (SkeletalMesh.GetDefaultMeshDeformer() != nullptr) Uses.Add({ MATUSAGE_MeshDeformer, {} });
		return Uses;
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
			ResetReportData();
			PackageUnloader.Reset();
			ProgressNotification.Show(
				LOCTEXT("PreparingMaterialAnalytics", "Preparing Material Analytics..."),
				LOCTEXT("CancelMaterialAnalyticsTooltip", "Stop gathering material analytics."),
				FSimpleDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::Cancel));

			// Work is split into small steps run on separate ticks, so the editor repaints the notification in between.
			Step = ERunnerStep::ShowNotification;
			StepStartSeconds = FPlatformTime::Seconds();
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::Tick));
		}

		/** Stops gathering and throws away unfinished results. */
		void Stop()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			TrackUnfinishedBatch();
			ResetReportData();
			PackageUnloader.UnloadReportPackages();
			ProgressNotification.Dismiss();
		}

	private:
		/** Stages of a report run. Each Tick performs one stage, or part of ProcessMaterials. */
		enum class ERunnerStep : uint8
		{
			/** Waits briefly so the notification is visible before any blocking work. */
			ShowNotification,
			/** Waits without blocking until the Asset Registry has finished its startup scan. */
			WaitForAssetRegistry,
			/** Scans the search folders and lists the master materials. */
			FindMaterials,
			/** Maps every material instance to its parent. */
			BuildInstanceTree,
			/** Fills report rows within a per-tick time budget. */
			ProcessMaterials,
			/** Unloads the packages loaded by the finished batch. */
			UnloadPackages
		};

		/** Updates the notification with the current material count and the last material processed. */
		void UpdateProgressNotification() const
		{
			const int32 ProgressPercent = PendingAssets.Num() > 0
				? FMath::RoundToInt(static_cast<float>(NextAssetIndex) / PendingAssets.Num() * 100.0f)
				: 100;
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("MaterialAnalyticsProgress", "Gathering Material Analytics - {0} / {1} ({2}%)\n{3}"),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::AsNumber(ProgressPercent),
				FText::FromString(CurrentAssetName)));
		}

		/** Clears pending assets, finished rows and the instance tree. */
		void ResetReportData()
		{
			PendingAssets.Reset();
			AssetItems.Reset();
			ChildInstancesByParent.Reset();
			InstancePackages.Reset();
			UserByPackage.Reset();
			UsesByUserPath.Reset();
			NextAssetIndex = 0;
			MaterialsInBatch = 0;
			BatchLoadedAssetCount = 0;
			PackagesBeforeBatch.Reset();
			CurrentAssetName.Reset();
		}

		/** Adds the packages loaded by a batch that was interrupted to the unloader, so they are not left loaded. */
		void TrackUnfinishedBatch()
		{
			if (MaterialsInBatch > 0 && BatchLoadedAssetCount > 0) PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
		}

		/** Cancels gathering, clears unfinished results, and updates the notification. */
		void Cancel()
		{
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
			TrackUnfinishedBatch();
			ResetReportData();
			PackageUnloader.UnloadReportPackages();
			ProgressNotification.Finish(
				LOCTEXT("MaterialAnalyticsCancelled", "Material Analytics gathering cancelled."),
				SNotificationItem::CS_None);
		}

		/**
		 * Maps every material instance in the project to its parent, without loading anything.
		 * Instances often live outside the search folders, so the whole registry is used; Tick waits for the
		 * registry's startup scan to finish before this runs.
		 */
		void BuildInstanceTree(IAssetRegistry& AssetRegistry)
		{
			TArray<FAssetData> MaterialInstances;
			AssetRegistry.GetAssetsByClass(
				UMaterialInstanceConstant::StaticClass()->GetClassPathName(),
				MaterialInstances,
				true);
			for (const FAssetData& MaterialInstance : MaterialInstances)
			{
				const FSoftObjectPath ParentPath(GetParentPath(MaterialInstance));
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
		 * Finds the distinct assets of each user type that reference the master or its counted instances.
		 * Material instances are skipped because the instance list already covers them.
		 * Output: Fills Item.UserPathsByType, one sorted list per EMaterialUserType.
		 */
		void GatherUsersByType(
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

			Item.UserPathsByType.SetNum(static_cast<int32>(EMaterialUserType::Count));
			for (const FName UserPackage : UserPackages)
			{
				const FMaterialUser* CachedUser = UserByPackage.Find(UserPackage);
				const FMaterialUser& User = CachedUser != nullptr
					? *CachedUser
					: UserByPackage.Add(UserPackage, MakeMaterialUser(AssetRegistry, UserPackage));
				Item.UserPathsByType[static_cast<int32>(User.Type)].Add(User.Path);
			}
			for (TArray<FString>& UserPaths : Item.UserPathsByType)
			{
				UserPaths.Sort();
			}
		}

		/** Scans the configured folders and lists their master materials in PendingAssets. */
		void FindMasterMaterials(IAssetRegistry& AssetRegistry)
		{
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

			TArray<FString> PathsToScan;
			for (const FName PackagePath : Filter.PackagePaths)
			{
				PathsToScan.Add(PackagePath.ToString());
			}
			AssetRegistry.ScanPathsSynchronous(PathsToScan, true);
			AssetRegistry.GetAssets(Filter, PendingAssets);
		}

		/** Returns whether the enabled settings need the material instance tree. */
		static bool NeedsInstanceTree(const UAssetAnalyticsSettings& Settings)
		{
			return Settings.bMaterialInstanceCounts || Settings.bMaterialUserTypeCounts || Settings.bMaterialShaderPermutations;
		}

		/** Returns whether the enabled settings load assets, so batches must be unloaded. */
		static bool NeedsAssetLoading(const UAssetAnalyticsSettings& Settings)
		{
			return Settings.bMaterialUsageFlags ||
				Settings.bMaterialShaderPermutations ||
				(Settings.bMaterialUserTypeCounts && Settings.bMaterialResolveUsersByLoading);
		}

		/**
		 * Loads a master and its counted instances to count their shader maps on the editor's shader platform.
		 * Instances that override static parameters or base properties compile their own shader maps; instances
		 * with identical overrides are still counted separately, although they may share cached shaders.
		 * Each of these materials compiles one shader map per quality level used by a Quality Switch, plus a default.
		 * Output: Fills the shader map fields of Item and sets Item.bShaderPermutationsRead.
		 * Returns: The number of materials loaded from disk.
		 */
		static int32 ReadStaticPermutations(const FAssetData& MasterData, FMaterialAssetData& Item)
		{
			int32 LoadedAssetCount = 0;
			const bool bWasMasterLoaded = MasterData.IsAssetLoaded();
			UMaterial* MasterMaterial = Cast<UMaterial>(MasterData.GetAsset());
			if (MasterMaterial == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material %s to count its shader maps."), *MasterData.GetObjectPathString());
				return LoadedAssetCount;
			}
			if (!bWasMasterLoaded) ++LoadedAssetCount;

			const EShaderPlatform ShaderPlatform = GMaxRHIShaderPlatform;
			const FQualityShaderMaps MasterQuality = GetQualityShaderMaps(*MasterMaterial, ShaderPlatform);
			for (const EMaterialQualityLevel::Type QualityLevel : MasterQuality.UsedLevels)
			{
				Item.QualityLevelsUsed.Add(LexToString(QualityLevel));
			}
			int32 ShaderMapCount = MasterQuality.ShaderMapCount;
			int32 UniqueShaderMapCount = MasterQuality.ShaderMapCount;
			// The cooked game for the editor's own OS, such as Windows, so editor-only shaders are left out of the layout.
			const ITargetPlatform* CookedPlatform = GetTargetPlatformManagerRef().FindTargetPlatform(FPlatformProperties::IniPlatformName());
			int32 LayoutShaderCount = CountLayoutShaders(*MasterMaterial, ShaderPlatform, CookedPlatform, 1, Item.LayoutShadersByVertexFactory);

			TArray<FMaterialParameterInfo> SwitchInfos;
			TArray<FGuid> SwitchGuids;
			MasterMaterial->GetAllParameterInfoOfType(EMaterialParameterType::StaticSwitch, SwitchInfos, SwitchGuids);
			for (const FMaterialParameterInfo& SwitchInfo : SwitchInfos)
			{
				FStaticSwitchSummary& Summary = Item.StaticSwitches.AddDefaulted_GetRef();
				Summary.ParameterInfo = SwitchInfo;
				FGuid DefaultExpressionGuid;
				MasterMaterial->GetStaticSwitchParameterDefaultValue(
					FHashedMaterialParameterInfo(SwitchInfo),
					Summary.bDefaultValue,
					DefaultExpressionGuid);
			}

			TArray<FStaticPermutationKey> PermutationKeys;
			for (const FAssetData& InstanceData : Item.Instances)
			{
				const bool bWasAssetLoaded = InstanceData.IsAssetLoaded();
				UMaterialInstance* Instance = Cast<UMaterialInstance>(InstanceData.GetAsset());
				if (Instance == nullptr)
				{
					UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material instance %s to check its shader map."), *InstanceData.GetObjectPathString());
					continue;
				}
				if (!bWasAssetLoaded) ++LoadedAssetCount;

				// Only the instance's own overrides count; overrides inherited from a parent instance are counted there.
				for (const FStaticSwitchParameter& SwitchParameter : Instance->GetStaticParameters().StaticSwitchParameters)
				{
					if (!SwitchParameter.bOverride) continue;
					FStaticSwitchSummary* Summary = Item.StaticSwitches.FindByPredicate(
						[&SwitchParameter](const FStaticSwitchSummary& Candidate)
						{
							return Candidate.ParameterInfo == SwitchParameter.ParameterInfo;
						});
					if (Summary != nullptr && SwitchParameter.Value != Summary->bDefaultValue) ++Summary->ChangedCount;
				}

				if (!Instance->bHasStaticPermutationResource) continue;

				Item.StaticPermutationInstancePaths.Add(InstanceData.GetObjectPathString());
				const int32 InstanceShaderMapCount = GetQualityShaderMaps(*Instance, ShaderPlatform).ShaderMapCount;
				ShaderMapCount += InstanceShaderMapCount;

				const FStaticPermutationReasons Reasons = FindStaticPermutationReasons(*Instance);
				if (Reasons.bStaticSwitches) ++Item.SwitchPermutationCount;
				if (Reasons.bMaterialLayers) ++Item.LayerPermutationCount;
				if (Reasons.bBaseProperties) ++Item.BasePropertyPermutationCount;
				if (!Reasons.bStaticSwitches && !Reasons.bMaterialLayers && !Reasons.bBaseProperties) ++Item.OtherPermutationCount;

				// Instances with equal effective static parameters and base property overrides compile the same shaders.
				FStaticPermutationKey Key;
				Instance->GetStaticParameterValues(Key.StaticParameters);
				Key.BasePropertyOverridesHash = Instance->GetBasePropertyOverridesHash();
				const bool bAlreadySeen = PermutationKeys.ContainsByPredicate([&Key](const FStaticPermutationKey& SeenKey)
				{
					return SeenKey.BasePropertyOverridesHash == Key.BasePropertyOverridesHash &&
						SeenKey.StaticParameters.Equivalent(Key.StaticParameters);
				});
				if (!bAlreadySeen)
				{
					PermutationKeys.Add(MoveTemp(Key));
					UniqueShaderMapCount += InstanceShaderMapCount;
					// The layout only covers the instance's active quality level, so it is repeated for each quality shader map.
					// GetShaderTypes asserts that static permutation instances are constant instances.
					if (Instance->IsA<UMaterialInstanceConstant>())
					{
						LayoutShaderCount += CountLayoutShaders(*Instance, ShaderPlatform, CookedPlatform, InstanceShaderMapCount, Item.LayoutShadersByVertexFactory);
					}
				}
			}
			Item.UniqueStaticPermutationCount = PermutationKeys.Num();
			Item.StaticPermutationInstancePaths.Sort();
			Item.ShaderMapCount = ShaderMapCount;
			Item.UniqueShaderMapCount = UniqueShaderMapCount;
			Item.LayoutShaderCount = LayoutShaderCount;
			Item.bShaderPermutationsRead = true;
			return LoadedAssetCount;
		}

		/**
		 * Loads AssetData's master material and records which usage flags it has enabled.
		 * Output: Fills Item.UsageEnabled, or leaves it empty when the material cannot be loaded.
		 * Returns: True when this call loaded the material from disk.
		 */
		static bool ReadUsageFlags(const FAssetData& AssetData, FMaterialAssetData& Item)
		{
			const bool bWasAssetLoaded = AssetData.IsAssetLoaded();
			const UMaterial* MasterMaterial = Cast<UMaterial>(AssetData.GetAsset());
			if (MasterMaterial == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material %s to read its usage flags."), *AssetData.GetObjectPathString());
				return false;
			}

			Item.UsageEnabled.SetNum(MATUSAGE_MAX);
			for (int32 UsageIndex = 0; UsageIndex < MATUSAGE_MAX; ++UsageIndex)
			{
				Item.UsageEnabled[UsageIndex] = MasterMaterial->GetUsageByFlag(static_cast<EMaterialUsage>(UsageIndex));
			}
			return !bWasAssetLoaded;
		}

		/**
		 * Returns the material uses of the user at UserPath, loading it the first time it is needed in this run.
		 * Output: Increases LoadedAssetCount when this call loaded the user from disk.
		 */
		const TArray<FMaterialUse>& GetUserUses(const FString& UserPath, int32& LoadedAssetCount)
		{
			if (const TArray<FMaterialUse>* CachedUses = UsesByUserPath.Find(UserPath)) return *CachedUses;

			const FSoftObjectPath UserObjectPath(UserPath);
			UObject* User = UserObjectPath.ResolveObject();
			if (User == nullptr)
			{
				User = UserObjectPath.TryLoad();
				if (User != nullptr) ++LoadedAssetCount;
			}

			TArray<FMaterialUse> Uses;
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(User)) Uses = FindBlueprintUses(*Blueprint);
			else if (const UParticleSystem* ParticleSystem = Cast<UParticleSystem>(User)) Uses = FindParticleSystemUses(*ParticleSystem);
			else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(User)) Uses = FindSkeletalMeshUses(*SkeletalMesh);
			else UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material user %s."), *UserPath);
			return UsesByUserPath.Add(UserPath, MoveTemp(Uses));
		}

		/**
		 * Loads the Blueprint, Cascade particle system and skeletal mesh users of a material and files each one
		 * under the usage flags it needs. Blueprints and particle systems whose uses of this material are all
		 * recognised are removed from their broad group; skeletal meshes stay under SkeletalMesh.
		 * Input: MaterialAndInstances is the master and its counted instances.
		 * Returns: The number of users loaded from disk.
		 */
		int32 ResolveUsersByLoading(FMaterialAssetData& Item, const TArray<FAssetData>& MaterialAndInstances)
		{
			TSet<FSoftObjectPath> TreeMaterials;
			for (const FAssetData& MaterialOrInstance : MaterialAndInstances)
			{
				TreeMaterials.Add(MaterialOrInstance.GetSoftObjectPath());
			}

			int32 LoadedAssetCount = 0;
			Item.ResolvedUserPathsByUsage.SetNum(MATUSAGE_MAX);
			const EMaterialUserType ResolvableTypes[] =
			{
				EMaterialUserType::Blueprint,
				EMaterialUserType::Particle,
				EMaterialUserType::SkeletalMesh
			};
			for (const EMaterialUserType UserType : ResolvableTypes)
			{
				TArray<FString>& UserPaths = Item.UserPathsByType[static_cast<int32>(UserType)];
				for (int32 UserIndex = UserPaths.Num() - 1; UserIndex >= 0; --UserIndex)
				{
					const FString UserPath = UserPaths[UserIndex];
					TSet<EMaterialUsage> NeededUsages;
					bool bHasUnrecognisedUse = false;
					for (const FMaterialUse& Use : GetUserUses(UserPath, LoadedAssetCount))
					{
						const bool bUsesThisMaterial = Use.Materials.Num() == 0 ||
							Use.Materials.ContainsByPredicate([&TreeMaterials](const FSoftObjectPath& Material)
							{
								return TreeMaterials.Contains(Material);
							});
						if (!bUsesThisMaterial) continue;
						if (Use.Usage == MATUSAGE_MAX) bHasUnrecognisedUse = true;
						else NeededUsages.Add(Use.Usage);
					}

					for (const EMaterialUsage Usage : NeededUsages)
					{
						Item.ResolvedUserPathsByUsage[Usage].AddUnique(UserPath);
					}
					const bool bFullyResolved = NeededUsages.Num() > 0 && !bHasUnrecognisedUse;
					if (UserType != EMaterialUserType::SkeletalMesh && bFullyResolved) UserPaths.RemoveAt(UserIndex);
				}
			}
			for (TArray<FString>& ResolvedUserPaths : Item.ResolvedUserPathsByUsage)
			{
				ResolvedUserPaths.Sort();
			}
			return LoadedAssetCount;
		}

		/**
		 * Fills the report row of one master material, loading it or its users when the settings ask for it.
		 * Returns: The number of assets this call loaded from disk.
		 */
		int32 ProcessMaterial(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings, const FAssetData& AssetData)
		{
			if (Settings.bIgnoreUnreferencedAssets &&
				!Core::HasRelevantReferencer(AssetRegistry, AssetData, Settings.bIgnoreDeveloperReferences)) return 0;

			int32 LoadedAssetCount = 0;
			FMaterialAssetData& Item = AssetItems.Emplace_GetRef(AssetData);
			if (Settings.bMaterialPackageDiskSize)
			{
				FAssetPackageData PackageData;
				if (AssetRegistry.TryGetAssetPackageData(AssetData.PackageName, PackageData) ==
					UE::AssetRegistry::EExists::Exists)
				{
					Item.PackageDiskSize = PackageData.DiskSize;
				}
			}
			if (NeedsInstanceTree(Settings))
			{
				Item.Instances = GatherCountedInstances(AssetRegistry, AssetData.GetSoftObjectPath(), Settings);
				if (Settings.bMaterialUserTypeCounts)
				{
					TArray<FAssetData> MaterialAndInstances = Item.Instances;
					MaterialAndInstances.Add(AssetData);
					GatherUsersByType(AssetRegistry, MaterialAndInstances, Settings, Item);
					if (Settings.bMaterialResolveUsersByLoading) LoadedAssetCount += ResolveUsersByLoading(Item, MaterialAndInstances);
				}
			}
			if (Settings.bMaterialUsageFlags && ReadUsageFlags(AssetData, Item)) ++LoadedAssetCount;
			if (Settings.bMaterialShaderPermutations) LoadedAssetCount += ReadStaticPermutations(AssetData, Item);
			return LoadedAssetCount;
		}

		/**
		 * Processes master materials until the tick time budget is used, so the editor keeps repainting.
		 * AssetsPerTick masters make one batch; when a batch has loaded assets, the next tick unloads them.
		 * Returns: True while the report is still running.
		 */
		bool ProcessMaterialsForOneTick(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings)
		{
			const double TickStartSeconds = FPlatformTime::Seconds();
			const bool bLoadsAssets = NeedsAssetLoading(Settings);
			const int32 MaterialsPerBatch = FMath::Max(Settings.AssetsPerTick, 1);
			while (NextAssetIndex < PendingAssets.Num())
			{
				// Snapshot loaded packages so everything this batch loads, including dependencies, can be unloaded afterwards.
				if (bLoadsAssets && MaterialsInBatch == 0)
				{
					PackagesBeforeBatch = Core::FReportPackageUnloader::GetLoadedPackages();
					BatchStartSeconds = FPlatformTime::Seconds();
					BatchLoadedAssetCount = 0;
				}

				const FAssetData& AssetData = PendingAssets[NextAssetIndex];
				CurrentAssetName = AssetData.AssetName.ToString();
				BatchLoadedAssetCount += ProcessMaterial(AssetRegistry, Settings, AssetData);
				++NextAssetIndex;
				++MaterialsInBatch;

				const bool bBatchFinished = MaterialsInBatch >= MaterialsPerBatch || NextAssetIndex >= PendingAssets.Num();
				if (bBatchFinished)
				{
					if (BatchLoadedAssetCount > 0)
					{
						// Unloading can wait on shader compiles, so it gets its own tick and message.
						PackageUnloader.TrackPackagesLoadedSince(PackagesBeforeBatch);
						Step = ERunnerStep::UnloadPackages;
						ProgressNotification.SetText(FText::Format(
							LOCTEXT("UnloadingMaterialPackages", "Material Analytics - {0} / {1}\nUnloading {2} packages..."),
							FText::AsNumber(NextAssetIndex),
							FText::AsNumber(PendingAssets.Num()),
							FText::AsNumber(PackageUnloader.GetTrackedPackageCount())));
						return true;
					}
					MaterialsInBatch = 0;
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
				Step = NeedsInstanceTree(*Settings) ? ERunnerStep::WaitForAssetRegistry : ERunnerStep::FindMaterials;
				return true;

			case ERunnerStep::WaitForAssetRegistry:
				// Polling instead of WaitForCompletion lets the registry keep scanning while the editor stays responsive.
				if (AssetRegistry.IsLoadingAssets())
				{
					ProgressNotification.SetText(LOCTEXT("WaitingForAssetRegistry", "Material Analytics\nWaiting for the Asset Registry to finish scanning..."));
					return true;
				}
				Step = ERunnerStep::FindMaterials;
				ProgressNotification.SetText(LOCTEXT("ScanningMaterialFolders", "Material Analytics\nScanning material folders..."));
				return true;

			case ERunnerStep::FindMaterials:
				FindMasterMaterials(AssetRegistry);
				if (PendingAssets.Num() == 0)
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				if (NeedsInstanceTree(*Settings))
				{
					Step = ERunnerStep::BuildInstanceTree;
					ProgressNotification.SetText(LOCTEXT("BuildingInstanceTree", "Material Analytics\nBuilding the material instance tree..."));
					return true;
				}
				Step = ERunnerStep::ProcessMaterials;
				UpdateProgressNotification();
				return true;

			case ERunnerStep::BuildInstanceTree:
				BuildInstanceTree(AssetRegistry);
				Step = ERunnerStep::ProcessMaterials;
				UpdateProgressNotification();
				return true;

			case ERunnerStep::ProcessMaterials:
				return ProcessMaterialsForOneTick(AssetRegistry, *Settings);

			case ERunnerStep::UnloadPackages:
				PackageUnloader.UnloadReportPackages();
				UE_LOG(
					LogTemp,
					Log,
					TEXT("Material Analytics loaded and unloaded %d assets (master materials and users) for %d materials in %.2f s."),
					BatchLoadedAssetCount,
					MaterialsInBatch,
					FPlatformTime::Seconds() - BatchStartSeconds);
				MaterialsInBatch = 0;
				BatchLoadedAssetCount = 0;
				if (NextAssetIndex >= PendingAssets.Num())
				{
					TickerHandle.Reset();
					CompleteReport();
					return false;
				}
				Step = ERunnerStep::ProcessMaterials;
				UpdateProgressNotification();
				return true;
			}
			return true;
		}

		/** Sorts the results, saves the JSON report, and opens the browser report. */
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
			const FString JsonPath = FPaths::Combine(ReportDirectory, TEXT("MaterialAnalytics.json"));
			if (!FFileHelper::SaveStringToFile(
				BuildJsonData(),
				*JsonPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				ProgressNotification.Finish(
					LOCTEXT("MaterialJsonSaveFailed", "The material analytics JSON file could not be saved."),
					SNotificationItem::CS_Fail);
				ResetReportData();
				return;
			}

			UE_LOG(LogTemp, Log, TEXT("Asset Analytics saved the Material Analytics report to %s"), *FPaths::ConvertRelativePathToFull(JsonPath));
			const FString ReportUrl = Core::GetMaterialAnalyticsReportUrl();
			FPlatformProcess::LaunchURL(*ReportUrl, nullptr, nullptr);
			ProgressNotification.Finish(
				FText::Format(
					LOCTEXT("MaterialAnalyticsComplete", "Material Analytics report saved for {0} master materials."),
					FText::AsNumber(AssetItems.Num())),
				SNotificationItem::CS_Success);
			ResetReportData();
		}

		/**
		 * Writes one material entry with the enabled fields.
		 * Output: Appends a JSON object to Writer, which must be inside the Materials array.
		 */
		static void WriteMaterialJson(
			TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>& Writer,
			const FMaterialAssetData& Item,
			const UAssetAnalyticsSettings& Settings)
		{
			Writer.WriteObjectStart();
			Writer.WriteValue(TEXT("AssetName"), Item.AssetData.AssetName.ToString());
			Writer.WriteValue(TEXT("PackagePath"), Item.AssetData.PackagePath.ToString());
			if (Settings.bMaterialPackageDiskSize)
			{
				if (Item.PackageDiskSize >= 0) Writer.WriteValue(TEXT("PackageDiskSize"), Item.PackageDiskSize);
				else Writer.WriteNull(TEXT("PackageDiskSize"));
			}
			if (Settings.bMaterialInstanceCounts)
			{
				Writer.WriteValue(TEXT("InstanceCount"), Item.Instances.Num());
				Writer.WriteArrayStart(TEXT("Instances"));
				for (const FAssetData& Instance : Item.Instances)
				{
					Writer.WriteValue(Instance.GetObjectPathString());
				}
				Writer.WriteArrayEnd();
			}
			if (Settings.bMaterialUsageFlags)
			{
				// False when the master could not be loaded, so missing Enabled values are not read as "disabled".
				Writer.WriteValue(TEXT("UsageFlagsRead"), Item.UsageEnabled.Num() > 0);
			}
			if (Item.bShaderPermutationsRead)
			{
				const int32 ShaderMapCount = Item.ShaderMapCount;
				Writer.WriteValue(TEXT("ShaderMapCount"), ShaderMapCount);
				Writer.WriteArrayStart(TEXT("QualityLevelsUsed"));
				for (const FString& QualityLevel : Item.QualityLevelsUsed)
				{
					Writer.WriteValue(QualityLevel);
				}
				Writer.WriteArrayEnd();
				// An instance can have several reasons, so these counts can add up to more than the static permutation instances.
				Writer.WriteObjectStart(TEXT("StaticPermutationReasons"));
				Writer.WriteValue(TEXT("StaticSwitches"), Item.SwitchPermutationCount);
				Writer.WriteValue(TEXT("MaterialLayers"), Item.LayerPermutationCount);
				Writer.WriteValue(TEXT("BaseProperties"), Item.BasePropertyPermutationCount);
				Writer.WriteValue(TEXT("Other"), Item.OtherPermutationCount);
				Writer.WriteObjectEnd();
				Writer.WriteValue(TEXT("UniqueStaticPermutations"), Item.UniqueStaticPermutationCount);
				// Writes a vertex factory breakdown as {Name: ShaderCount}, largest first.
				auto WriteShadersByVertexFactory = [&Writer](const TCHAR* FieldName, TMap<FString, int32> ShadersByVertexFactory)
				{
					ShadersByVertexFactory.ValueSort(TGreater<int32>());
					Writer.WriteObjectStart(FieldName);
					for (const TPair<FString, int32>& VertexFactoryShaders : ShadersByVertexFactory)
					{
						Writer.WriteValue(VertexFactoryShaders.Key, VertexFactoryShaders.Value);
					}
					Writer.WriteObjectEnd();
				};

				// Shaders of the master and one instance per unique static permutation, on the editor's shader platform
				// with the cooked game's permutations.
				Writer.WriteValue(TEXT("ShaderCount"), Item.LayoutShaderCount);
				WriteShadersByVertexFactory(TEXT("ShaderCountByVertexFactory"), Item.LayoutShadersByVertexFactory);

				// ChangedCount: counted instances overriding the switch with the value opposite to its default.
				Writer.WriteArrayStart(TEXT("StaticSwitches"));
				for (const FStaticSwitchSummary& StaticSwitch : Item.StaticSwitches)
				{
					Writer.WriteObjectStart();
					Writer.WriteValue(TEXT("Name"), StaticSwitch.ParameterInfo.Name.ToString());
					Writer.WriteValue(TEXT("Default"), StaticSwitch.bDefaultValue);
					Writer.WriteValue(TEXT("ChangedCount"), StaticSwitch.ChangedCount);
					Writer.WriteObjectEnd();
				}
				Writer.WriteArrayEnd();
				Writer.WriteArrayStart(TEXT("StaticPermutationInstances"));
				for (const FString& InstancePath : Item.StaticPermutationInstancePaths)
				{
					Writer.WriteValue(InstancePath);
				}
				Writer.WriteArrayEnd();

				// A rough indicator only: every unique shader map compiles shaders for every enabled usage flag.
				// Instances with the same static permutation share their shaders when cooked, so they are counted once.
				if (Item.UsageEnabled.Num() > 0)
				{
					int32 EnabledUsageCount = 0;
					for (const bool bUsageEnabled : Item.UsageEnabled)
					{
						if (bUsageEnabled) ++EnabledUsageCount;
					}
					Writer.WriteValue(TEXT("EstimatedVertexFactoryPermutations"), Item.UniqueShaderMapCount * EnabledUsageCount);
				}
			}
			if (Settings.bMaterialUserTypeCounts || Settings.bMaterialUsageFlags)
			{
				Writer.WriteObjectStart(TEXT("VertexFactories"));
				for (const FVertexFactoryEntry& Entry : VertexFactoryEntries)
				{
					WriteVertexFactoryJson(Writer, Entry, Item);
				}
				Writer.WriteObjectEnd();
			}
			Writer.WriteObjectEnd();
		}

		/**
		 * Writes one VertexFactories entry when it has something to show: users, or an enabled usage flag.
		 * Users is written only when the registry can find users of this type, so an empty list means "enabled
		 * but unused" and a missing Users key means the users are unknown without loading them.
		 * Output: Appends Entry.Name and its object to Writer, or nothing when the entry is skipped.
		 */
		static void WriteVertexFactoryJson(
			TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>& Writer,
			const FVertexFactoryEntry& Entry,
			const FMaterialAssetData& Item)
		{
			const bool bHasUsageFlag = Entry.Usage != MATUSAGE_MAX && Item.UsageEnabled.Num() > 0;
			const bool bEnabled = bHasUsageFlag && Item.UsageEnabled[Entry.Usage];
			const bool bRegistryUsersKnown =
				Entry.RegistryUserType != EMaterialUserType::Count && Item.UserPathsByType.Num() > 0;

			// Users found by loading are added to the registry users. They only make Users known for entries the
			// registry cannot fill when at least one was found, because levels and placements were not checked.
			TArray<FString> UserPaths;
			if (bRegistryUsersKnown) UserPaths = Item.UserPathsByType[static_cast<int32>(Entry.RegistryUserType)];
			if (Entry.Usage != MATUSAGE_MAX && Item.ResolvedUserPathsByUsage.Num() > 0)
			{
				for (const FString& ResolvedUserPath : Item.ResolvedUserPathsByUsage[Entry.Usage])
				{
					UserPaths.AddUnique(ResolvedUserPath);
				}
				UserPaths.Sort();
			}
			const bool bUsersKnown = bRegistryUsersKnown || UserPaths.Num() > 0;
			if (UserPaths.Num() == 0 && !bEnabled) return;

			Writer.WriteObjectStart(Entry.Name);
			if (bHasUsageFlag) Writer.WriteValue(TEXT("Enabled"), bEnabled);
			if (bUsersKnown)
			{
				Writer.WriteArrayStart(TEXT("Users"));
				for (const FString& UserPath : UserPaths)
				{
					Writer.WriteValue(UserPath);
				}
				Writer.WriteArrayEnd();
			}
			Writer.WriteObjectEnd();
		}

		/** Returns the JSON report text for the finished materials and the enabled fields. */
		FString BuildJsonData() const
		{
			const UAssetAnalyticsSettings* Settings = GetDefault<UAssetAnalyticsSettings>();
			FString JsonText;
			const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
			Writer->WriteObjectStart();
			Writer->WriteArrayStart(TEXT("Materials"));
			for (const FMaterialAssetData& Item : AssetItems)
			{
				WriteMaterialJson(*Writer, Item, *Settings);
			}
			Writer->WriteArrayEnd();
			Writer->WriteObjectEnd();
			Writer->Close();
			return JsonText;
		}

		static constexpr double NotificationDisplayDelaySeconds = 0.2;
		/** Longest time one tick spends processing materials before letting the editor repaint. */
		static constexpr double TickTimeBudgetSeconds = 0.05;
		TArray<FMaterialAssetData> AssetItems;
		TArray<FAssetData> PendingAssets;
		/** Direct child instances for every material or instance that has any, keyed by the parent's object path. */
		TMap<FSoftObjectPath, TArray<FAssetData>> ChildInstancesByParent;
		/** Packages of every material instance in the instance tree, so they are not counted as users. */
		TSet<FName> InstancePackages;
		/** Type and report path of each referencing package classified so far in this run. */
		TMap<FName, FMaterialUser> UserByPackage;
		/** Material uses of each user loaded so far in this run, so every user is loaded only once. */
		TMap<FString, TArray<FMaterialUse>> UsesByUserPath;
		/** Unloads the master materials loaded to read usage flags, after each batch. */
		Core::FReportPackageUnloader PackageUnloader;
		FTSTicker::FDelegateHandle TickerHandle;
		/** Notification that shows the report progress and the Cancel button. */
		Core::FReportProgressNotification ProgressNotification;
		int32 NextAssetIndex = 0;
		/** Stage the next Tick runs. */
		ERunnerStep Step = ERunnerStep::ShowNotification;
		/** When the report started, used to delay blocking work until the notification is visible. */
		double StepStartSeconds = 0.0;
		/** Materials processed in the current batch. */
		int32 MaterialsInBatch = 0;
		/** Assets loaded from disk in the current batch. */
		int32 BatchLoadedAssetCount = 0;
		/** When the current batch started, for the batch timing log. */
		double BatchStartSeconds = 0.0;
		/** Packages that were loaded before the current batch started. */
		TSet<UPackage*> PackagesBeforeBatch;
		/** Name of the last material processed, shown in the notification. */
		FString CurrentAssetName;
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
