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
#include "Hash/CityHash.h"
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
#include "MaterialShader.h"
#include "MeshMaterialShader.h"
#include "RHI.h"
#include "SceneTypes.h"
#include "Shader.h"
#include "ShaderPlatformQualitySettings.h"
#include "StaticParameterSet.h"
#include "VertexFactory.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/LinkerLoad.h"
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
		VirtualHeightfieldMesh,
		Level,
		Blueprint,
		Other,
		/** Also used in the vertex factory table for entries whose users cannot be found without loading. */
		Count
	};

	/** How the users of a vertex factory entry are found when the Asset Registry cannot list them. */
	enum class EUnregisteredUserLookup : uint8
	{
		/** Loading Blueprint, Cascade particle system and skeletal mesh users can find them. */
		Loading,
		/** Users are actors placed in levels, which the report does not check. */
		PlacedActors,
		/** Users are Niagara systems, which the report does not load. */
		Niagara,
		/** The report has no way to find these users. */
		NotChecked
	};

	/** One entry of the VertexFactories object in the JSON report. */
	struct FVertexFactoryEntry
	{
		/** JSON key: the engine's EMaterialUsage name without MATUSAGE_, or a user type that has no usage flag. */
		const TCHAR* Name;

		/** Registry user type listed under this entry, or Count when the registry cannot find its users. */
		EMaterialUserType RegistryUserType;

		/** Engine usage flag this entry reports as Enabled, or MATUSAGE_MAX for entries that are not usage flags. */
		EMaterialUsage Usage;

		/** How users are found when RegistryUserType is Count. Unused for registry entries. */
		EUnregisteredUserLookup UnregisteredUserLookup = EUnregisteredUserLookup::NotChecked;
	};

	/**
	 * Every entry written under VertexFactories: all UE 4.27 EMaterialUsage flags in engine order,
	 * then the user types that do not match a single usage flag. UE 4.27 has no static mesh or volumetric cloud
	 * usage flag, so those two only list their registry users.
	 */
	static const FVertexFactoryEntry VertexFactoryEntries[] =
	{
		{ TEXT("SkeletalMesh"), EMaterialUserType::SkeletalMesh, MATUSAGE_SkeletalMesh },
		{ TEXT("ParticleSprites"), EMaterialUserType::Count, MATUSAGE_ParticleSprites, EUnregisteredUserLookup::Loading },
		{ TEXT("BeamTrails"), EMaterialUserType::Count, MATUSAGE_BeamTrails, EUnregisteredUserLookup::Loading },
		{ TEXT("MeshParticles"), EMaterialUserType::Count, MATUSAGE_MeshParticles, EUnregisteredUserLookup::Loading },
		{ TEXT("StaticLighting"), EMaterialUserType::Count, MATUSAGE_StaticLighting, EUnregisteredUserLookup::PlacedActors },
		{ TEXT("MorphTargets"), EMaterialUserType::Count, MATUSAGE_MorphTargets, EUnregisteredUserLookup::Loading },
		{ TEXT("SplineMesh"), EMaterialUserType::Count, MATUSAGE_SplineMesh, EUnregisteredUserLookup::Loading },
		{ TEXT("InstancedStaticMeshes"), EMaterialUserType::InstancedStaticMeshes, MATUSAGE_InstancedStaticMeshes },
		{ TEXT("GeometryCollections"), EMaterialUserType::GeometryCollections, MATUSAGE_GeometryCollections },
		{ TEXT("Clothing"), EMaterialUserType::Count, MATUSAGE_Clothing, EUnregisteredUserLookup::Loading },
		{ TEXT("NiagaraSprites"), EMaterialUserType::Count, MATUSAGE_NiagaraSprites, EUnregisteredUserLookup::Niagara },
		{ TEXT("NiagaraRibbons"), EMaterialUserType::Count, MATUSAGE_NiagaraRibbons, EUnregisteredUserLookup::Niagara },
		{ TEXT("NiagaraMeshParticles"), EMaterialUserType::Count, MATUSAGE_NiagaraMeshParticles, EUnregisteredUserLookup::Niagara },
		{ TEXT("GeometryCache"), EMaterialUserType::GeometryCache, MATUSAGE_GeometryCache },
		{ TEXT("Water"), EMaterialUserType::Water, MATUSAGE_Water },
		{ TEXT("HairStrands"), EMaterialUserType::HairStrands, MATUSAGE_HairStrands },
		{ TEXT("LidarPointCloud"), EMaterialUserType::LidarPointCloud, MATUSAGE_LidarPointCloud },
		{ TEXT("VirtualHeightfieldMesh"), EMaterialUserType::VirtualHeightfieldMesh, MATUSAGE_VirtualHeightfieldMesh },
		{ TEXT("VolumetricCloud"), EMaterialUserType::VolumetricCloud, MATUSAGE_MAX },
		{ TEXT("StaticMesh"), EMaterialUserType::StaticMesh, MATUSAGE_MAX },
		{ TEXT("Niagara"), EMaterialUserType::Niagara, MATUSAGE_MAX },
		{ TEXT("Particle"), EMaterialUserType::Particle, MATUSAGE_MAX },
		{ TEXT("Level"), EMaterialUserType::Level, MATUSAGE_MAX },
		{ TEXT("Blueprint"), EMaterialUserType::Blueprint, MATUSAGE_MAX },
		{ TEXT("Other"), EMaterialUserType::Other, MATUSAGE_MAX },
	};

	/** A classified asset that references a material or one of its instances. */
	struct FMaterialUser
	{
		/** Kind of asset or actor that uses the material. */
		EMaterialUserType Type = EMaterialUserType::Other;

		/** Object path shown in the report, or the package name when the package has no registry asset. */
		FString Path;
	};

	/** Kinds of instance overrides that give an instance its own shader map. */
	enum class EPermutationParameterKind : uint8
	{
		StaticSwitch,
		ComponentMask,
		BaseProperty
	};

	/** One static parameter or base property of a master and how its counted instances override it. */
	struct FPermutationParameterSummary
	{
		/** Whether this is a static switch, a static component mask or a base property such as blend mode. */
		EPermutationParameterKind Kind = EPermutationParameterKind::StaticSwitch;

		/** Identifies the parameter, including its layer when it comes from a material layer. Base properties only set Name. */
		FMaterialParameterInfo ParameterInfo;

		/** Value the master uses when an instance does not override it, as report text. */
		FString DefaultValue;

		/** Counted instances whose own override gives a value different from their parent's. */
		int32 ChangedCount = 0;

		/** Counted instances whose own override repeats the value they would inherit from their parent. */
		int32 NoOpCount = 0;
	};

	/** A static parameter or base property of one instance that the instance overrides or that differs from the master. */
	struct FInstancePermutationCell
	{
		/** Index of the parameter in FMaterialAssetData::PermutationParameters. */
		int32 ParameterIndex = INDEX_NONE;

		/** Value the instance ends up with, as report text. */
		FString Value;

		/** Whether the instance's own overrides set this parameter, rather than inheriting it from its parent. */
		bool bOwnOverride = false;

		/** Whether the instance's own override repeats the value it would inherit from its parent. */
		bool bSameAsParent = false;
	};

	/** Static parameters of one counted instance. */
	struct FInstancePermutationRow
	{
		/** Object path of the instance. */
		FString Path;

		/** Parameters the instance overrides or ends up with a value different from the master's. */
		TArray<FInstancePermutationCell> Cells;
	};

	/** Everything the report gathers for one master material. */
	struct FMaterialAssetData
	{
		/** Creates an empty report row for InAssetData. */
		explicit FMaterialAssetData(const FAssetData& InAssetData)
			: AssetData(InAssetData)
		{
		}

		/** Registry entry of the master material. */
		FAssetData AssetData;

		/** Size of the master's package on disk in bytes, or -1 when unknown. */
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

		/** Shaders the master and one instance per unique static permutation compile, from the engine's shader map layout. */
		int32 LayoutShaderCount = INDEX_NONE;

		/** Static switches, component masks and base properties of the master, with how counted instances override them. */
		TArray<FPermutationParameterSummary> PermutationParameters;

		/** Static parameters of every loaded counted instance, sorted by path. */
		TArray<FInstancePermutationRow> InstancePermutationRows;
	};

	/**
	 * Effective static parameters and base property overrides of a material, used to group materials that compile
	 * the same shader map. Matches what the engine puts in a material's shader map ID.
	 */
	struct FStaticPermutationKey
	{
		/** Static parameters the material ends up with, including inherited ones, with every bOverride cleared. */
		FStaticParameterSet StaticParameters;

		/** Hash of the base properties that differ from the master's, such as blend mode or two sided; 0 when none do. */
		uint64 BasePropertyOverridesHash = 0;

		/**
		 * Returns whether Other compiles the same shader map as this key. UE 4.27 does not export
		 * FStaticParameterSet::Equivalent, so the static switches and component masks are compared here the same way:
		 * as sets, ignoring order. Both keys come from MakePermutationKey, so their bOverride flags are all cleared.
		 */
		bool CompilesSameShaders(const FStaticPermutationKey& Other) const
		{
			auto HaveSameParameters = [](const auto& Left, const auto& Right)
			{
				if (Left.Num() != Right.Num()) return false;
				for (const auto& Parameter : Left)
				{
					if (!Right.Contains(Parameter)) return false;
				}
				return true;
			};
			return BasePropertyOverridesHash == Other.BasePropertyOverridesHash &&
				HaveSameParameters(StaticParameters.StaticSwitchParameters, Other.StaticParameters.StaticSwitchParameters) &&
				HaveSameParameters(StaticParameters.StaticComponentMaskParameters, Other.StaticParameters.StaticComponentMaskParameters);
		}
	};

	/** A base property an instance can override, read through UMaterialInterface so inherited values are included. */
	struct FBasePropertyCheck
	{
		/** Property name shown in the report. */
		const TCHAR* Name;

		/** Returns whether the instance's own overrides set this property. */
		bool (*IsOverridden)(const FMaterialInstanceBasePropertyOverrides& Overrides);

		/** Returns whether the two materials end up with the same value for this property. */
		bool (*HasSameValue)(const UMaterialInterface& Left, const UMaterialInterface& Right);

		/** Returns Seed combined with the raw bytes of the material's value for this property. */
		uint64 (*HashValue)(const UMaterialInterface& Material, uint64 Seed);

		/** Returns the material's value for this property as report text, such as "Masked" or "True". */
		FString (*ValueText)(const UMaterialInterface& Material);
	};

	/** Compares two base property values the way UMaterialInstance::GetBasePropertyOverridesHash does. */
	template <typename ValueType>
	static bool HaveSameBasePropertyValue(const ValueType& Left, const ValueType& Right)
	{
		return !(Left != Right);
	}

	/** Compares floating point base properties with the engine's tolerance. Picked over the template for float values. */
	static bool HaveSameBasePropertyValue(const float Left, const float Right)
	{
		return FMath::IsNearlyEqual(Left, Right, SMALL_NUMBER);
	}

	/**
	 * Returns whether Material casts dynamic shadows as masked. UE 4.27's UMaterialInstance does not override
	 * GetCastDynamicShadowAsMasked, so instances read the value they cache from their overrides and parent.
	 */
	static bool GetCastDynamicShadowAsMaskedValue(const UMaterialInterface& Material)
	{
		if (const UMaterialInstance* Instance = Cast<UMaterialInstance>(&Material)) return Instance->bCastDynamicShadowAsMasked;
		return Material.GetCastDynamicShadowAsMasked();
	}

	/** Formats a base property value as report text. One overload per value type the base property getters return. */
	static FString FormatBasePropertyValue(const bool bValue) { return bValue ? TEXT("True") : TEXT("False"); }
	static FString FormatBasePropertyValue(const float Value) { return FString::SanitizeFloat(Value, 0); }

	static FString FormatBasePropertyValue(const EBlendMode BlendMode)
	{
		return StaticEnum<EBlendMode>()->GetDisplayNameTextByValue(BlendMode).ToString();
	}

	/** Lists every shading model in the field, such as "Default Lit, Subsurface". */
	static FString FormatBasePropertyValue(const FMaterialShadingModelField ShadingModels)
	{
		TArray<FString> ShadingModelNames;
		for (int32 ShadingModel = 0; ShadingModel < MSM_NUM; ++ShadingModel)
		{
			if (!ShadingModels.HasShadingModel(static_cast<EMaterialShadingModel>(ShadingModel))) continue;
			ShadingModelNames.Add(StaticEnum<EMaterialShadingModel>()->GetDisplayNameTextByValue(ShadingModel).ToString());
		}
		return FString::Join(ShadingModelNames, TEXT(", "));
	}

	/**
	 * Builds one BasePropertyChecks entry. ReadValue is an expression that reads the property from a
	 * const UMaterialInterface& named Material, such as Material.GetBlendMode().
	 */
#define ASSET_ANALYTICS_BASE_PROPERTY(Name, OverrideFlag, ReadValue) \
	{ \
		TEXT(Name), \
		[](const FMaterialInstanceBasePropertyOverrides& Overrides) { return Overrides.OverrideFlag != 0; }, \
		[](const UMaterialInterface& Left, const UMaterialInterface& Right) \
		{ \
			auto Read = [](const UMaterialInterface& Material) { return ReadValue; }; \
			return HaveSameBasePropertyValue(Read(Left), Read(Right)); \
		}, \
		[](const UMaterialInterface& Material, const uint64 Seed) \
		{ \
			const auto Value = ReadValue; \
			return CityHash64WithSeed(reinterpret_cast<const char*>(&Value), sizeof(Value), Seed); \
		}, \
		[](const UMaterialInterface& Material) { return FormatBasePropertyValue(ReadValue); } \
	}

	/** The base properties UE 4.27's UMaterialInstance::GetBasePropertyOverridesHash puts in a shader map ID. */
	static const FBasePropertyCheck BasePropertyChecks[] =
	{
		ASSET_ANALYTICS_BASE_PROPERTY("OpacityMaskClipValue", bOverride_OpacityMaskClipValue, Material.GetOpacityMaskClipValue()),
		ASSET_ANALYTICS_BASE_PROPERTY("BlendMode", bOverride_BlendMode, Material.GetBlendMode()),
		ASSET_ANALYTICS_BASE_PROPERTY("ShadingModel", bOverride_ShadingModel, Material.GetShadingModels()),
		ASSET_ANALYTICS_BASE_PROPERTY("TwoSided", bOverride_TwoSided, Material.IsTwoSided()),
		ASSET_ANALYTICS_BASE_PROPERTY("DitheredLODTransition", bOverride_DitheredLODTransition, Material.IsDitheredLODTransition()),
		ASSET_ANALYTICS_BASE_PROPERTY("CastDynamicShadowAsMasked", bOverride_CastDynamicShadowAsMasked, GetCastDynamicShadowAsMaskedValue(Material)),
	};

	/**
	 * Hashes the base properties whose value in Instance differs from its master's, as the engine does when it builds
	 * a shader map ID. UE 4.27 does not export UMaterialInstance::GetBasePropertyOverridesHash, so the plugin computes
	 * its own hash. It only needs equal overrides to give equal hashes; it does not match the engine's value.
	 * Returns: 0 when no base property differs from the master.
	 */
	static uint64 GetBasePropertyOverridesHash(const UMaterialInstance& Instance)
	{
		const UMaterial* Master = Instance.GetMaterial();
		if (Master == nullptr) return 0;

		uint64 Hash = 0;
		for (int32 PropertyIndex = 0; PropertyIndex < UE_ARRAY_COUNT(BasePropertyChecks); ++PropertyIndex)
		{
			const FBasePropertyCheck& BaseProperty = BasePropertyChecks[PropertyIndex];
			if (BaseProperty.HasSameValue(Instance, *Master)) continue;
			// Seeding with the property index keeps equal values of different properties apart.
			Hash = BaseProperty.HashValue(Instance, Hash + PropertyIndex + 1);
		}
		return Hash;
	}

#undef ASSET_ANALYTICS_BASE_PROPERTY

	/**
	 * Counts the shader maps Material compiles across quality levels on ShaderPlatform when cooking: one per quality
	 * level whose Quality Switch inputs differ or that a platform quality override forces, plus one default.
	 * It repeats UMaterialInterface::GetQualityLevelUsage (not exported from the Engine module) and
	 * UMaterial::GetNewResources: each used quality level gets its own shader map, and the unused levels share a
	 * default one unless the platform discards them when cooking.
	 */
	static int32 CountQualityShaderMaps(const UMaterialInterface& Material, const EShaderPlatform ShaderPlatform)
	{
		// Quality levels whose Quality Switch inputs differ, including switches inside material functions.
		// UE 4.27 caches them on the master only; its instances use the master's, as the 4.27 cook does.
		const UMaterial* BaseMaterial = Material.GetMaterial();
		if (BaseMaterial == nullptr) return 1;
		const TArray<bool>& SwitchQualityLevelsUsed = BaseMaterial->GetCachedExpressionData().QualityLevelsUsed;
		const UShaderPlatformQualitySettings* QualitySettings =
			UMaterialShaderQualitySettings::Get()->GetShaderPlatformQualitySettings(ShaderPlatform);

		int32 ShaderMapCount = 0;
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
				++ShaderMapCount;
			}
			else if (!bDiscardedWhenCooking)
			{
				bNeedsDefaultShaderMap = true;
			}
		}
		if (bNeedsDefaultShaderMap) ++ShaderMapCount;
		return ShaderMapCount;
	}

	/** Returns whether every stage of PipelineType compiles for Parameters, so the pipeline is part of the shader map. */
	static bool ShouldCompilePipeline(const FShaderPipelineType& PipelineType, const FShaderPermutationParameters& Parameters)
	{
		for (const FShaderType* StageType : PipelineType.GetStages())
		{
			if (!StageType->ShouldCompilePermutation(Parameters)) return false;
		}
		return true;
	}

	/**
	 * Counts the shaders Material's shader map compiles on ShaderPlatform, from its shader map layout. Nothing is
	 * compiled. UE 4.27 keeps the layout code private (FMaterialShaderMapLayoutCache::CreateLayout in
	 * MaterialShader.cpp), so this repeats it with the exported shader type APIs: every material shader type and
	 * pipeline, then the mesh material shader types and pipelines of each vertex factory the material caches.
	 * Like a cook, editor-only permutations are left out, and FMaterial::ShouldCache filters shader types as real
	 * compiles do. The layout covers the material's active quality level only.
	 * Inputs: Material needs a material resource for the editor's feature level, which the editor creates on load.
	 * Multiplier repeats the count, for the quality shader maps the layout does not cover.
	 * Returns: The shader count, or 0 when Material has no material resource.
	 */
	static int32 CountLayoutShaders(UMaterialInterface& Material, const EShaderPlatform ShaderPlatform, const int32 Multiplier)
	{
		const FMaterialResource* MaterialResource = Material.GetMaterialResource(GMaxRHIFeatureLevel);
		if (MaterialResource == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not count the shaders of %s because it has no material resource."), *Material.GetPathName());
			return 0;
		}

		const FMaterialShaderParameters MaterialParameters(MaterialResource);
		// Without the HasEditorOnlyData flag the layout matches a cooked game.
		const EShaderPermutationFlags PermutationFlags = EShaderPermutationFlags::None;
		const bool bHasTessellation = MaterialParameters.TessellationMode != MTM_NoTessellation;
		const bool bSupportsPipelines = RHISupportsShaderPipelines(ShaderPlatform);

		int32 ShaderCount = 0;
		for (FShaderType* ShaderType : FShaderType::GetSortedTypes(FShaderType::EShaderTypeForDynamicCast::Material))
		{
			if (!MaterialResource->ShouldCache(ShaderPlatform, ShaderType, nullptr)) continue;
			for (int32 PermutationId = 0; PermutationId < ShaderType->GetPermutationCount(); ++PermutationId)
			{
				const FMaterialShaderPermutationParameters Parameters(ShaderPlatform, MaterialParameters, PermutationId, PermutationFlags);
				if (ShaderType->ShouldCompilePermutation(Parameters)) ++ShaderCount;
			}
		}
		if (bSupportsPipelines)
		{
			const FMaterialShaderPermutationParameters Parameters(ShaderPlatform, MaterialParameters, kUniqueShaderPermutationId, PermutationFlags);
			for (const FShaderPipelineType* PipelineType : FShaderPipelineType::GetSortedTypes(FShaderType::EShaderTypeForDynamicCast::Material))
			{
				if (PipelineType->HasTessellation() == bHasTessellation && ShouldCompilePipeline(*PipelineType, Parameters))
				{
					ShaderCount += PipelineType->GetStages().Num();
				}
			}
		}

		for (FVertexFactoryType* VertexFactoryType : FVertexFactoryType::GetSortedMaterialTypes())
		{
			const FVertexFactoryShaderPermutationParameters VertexFactoryParameters(ShaderPlatform, MaterialParameters, VertexFactoryType, PermutationFlags);
			if (!VertexFactoryType->ShouldCache(VertexFactoryParameters)) continue;

			for (FShaderType* ShaderType : FShaderType::GetSortedTypes(FShaderType::EShaderTypeForDynamicCast::MeshMaterial))
			{
				if (!MaterialResource->ShouldCache(ShaderPlatform, ShaderType, VertexFactoryType)) continue;
				for (int32 PermutationId = 0; PermutationId < ShaderType->GetPermutationCount(); ++PermutationId)
				{
					const FMeshMaterialShaderPermutationParameters Parameters(ShaderPlatform, MaterialParameters, VertexFactoryType, PermutationId, PermutationFlags);
					if (ShaderType->ShouldCompilePermutation(Parameters)) ++ShaderCount;
				}
			}
			if (bSupportsPipelines)
			{
				const FMeshMaterialShaderPermutationParameters Parameters(ShaderPlatform, MaterialParameters, VertexFactoryType, kUniqueShaderPermutationId, PermutationFlags);
				for (const FShaderPipelineType* PipelineType : FShaderPipelineType::GetSortedTypes(FShaderType::EShaderTypeForDynamicCast::MeshMaterial))
				{
					if (PipelineType->HasTessellation() == bHasTessellation && ShouldCompilePipeline(*PipelineType, Parameters))
					{
						ShaderCount += PipelineType->GetStages().Num();
					}
				}
			}
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
		// UE 4.27 registry entries only hold short class names. Plugin classes are compared by name, so their
		// modules are not needed as dependencies.
		static const FName InstancedFoliageActorClass(TEXT("InstancedFoliageActor"));
		static const FName WaterBodyClass(TEXT("WaterBody"));
		static const FName VolumetricCloudClass(TEXT("VolumetricCloud"));
		static const FName VirtualHeightfieldMeshClass(TEXT("VirtualHeightfieldMesh"));

		// Check the actor class and its parents, so subclasses such as WaterBodyRiver match too.
		TArray<FName> ActorClassAndAncestors;
		AssetRegistry.GetAncestorClassNames(ActorAsset.AssetClass, ActorClassAndAncestors);
		ActorClassAndAncestors.Add(ActorAsset.AssetClass);
		for (const FName ClassName : ActorClassAndAncestors)
		{
			if (ClassName == InstancedFoliageActorClass) return EMaterialUserType::InstancedStaticMeshes;
			if (ClassName == WaterBodyClass) return EMaterialUserType::Water;
			if (ClassName == VolumetricCloudClass) return EMaterialUserType::VolumetricCloud;
			if (ClassName == VirtualHeightfieldMeshClass) return EMaterialUserType::VirtualHeightfieldMesh;
		}
		return EMaterialUserType::Level;
	}

	/**
	 * Decides which kind of asset a referencing package holds, using only registry data.
	 * Inputs: ReferencerPackage is a package that references a material or material instance, and PackageAssets
	 * are the registry assets in that package.
	 * Returns: The matching user type, or Other when no rule matches.
	 */
	static EMaterialUserType ClassifyReferencer(
		IAssetRegistry& AssetRegistry,
		const FName ReferencerPackage,
		const TArray<FAssetData>& PackageAssets)
	{
		// World Partition stores each placed actor in its own package under __ExternalActors__.
		// UE 4.27 has no FPackagePath, so the folder name is written out.
		const FString PackageName = ReferencerPackage.ToString();
		if (PackageName.Contains(TEXT("/__ExternalActors__/")))
		{
			return PackageAssets.Num() > 0
				? ClassifyPlacedActor(AssetRegistry, PackageAssets[0])
				: EMaterialUserType::Level;
		}
		if (PackageAssets.Num() == 0) return EMaterialUserType::Other;

		// UE 4.27 registry entries only hold short class names. Plugin classes are compared by name, so their
		// modules are not needed as dependencies.
		static const FName StaticMeshClass(TEXT("StaticMesh"));
		static const FName SkeletalMeshClass(TEXT("SkeletalMesh"));
		static const FName NiagaraSystemClass(TEXT("NiagaraSystem"));
		static const FName ParticleSystemClass(TEXT("ParticleSystem"));
		static const FName GeometryCollectionClass(TEXT("GeometryCollection"));
		static const FName GeometryCacheClass(TEXT("GeometryCache"));
		static const FName GroomClass(TEXT("GroomAsset"));
		static const FName LidarPointCloudClass(TEXT("LidarPointCloud"));
		static const FName WorldClass(TEXT("World"));

		const FAssetData& ReferencerAsset = PackageAssets[0];
		const FName ClassName = ReferencerAsset.AssetClass;
		if (ClassName == StaticMeshClass) return EMaterialUserType::StaticMesh;
		if (ClassName == SkeletalMeshClass) return EMaterialUserType::SkeletalMesh;
		if (ClassName == NiagaraSystemClass) return EMaterialUserType::Niagara;
		if (ClassName == ParticleSystemClass) return EMaterialUserType::Particle;
		if (ClassName == GeometryCollectionClass) return EMaterialUserType::GeometryCollections;
		if (ClassName == GeometryCacheClass) return EMaterialUserType::GeometryCache;
		if (ClassName == GroomClass) return EMaterialUserType::HairStrands;
		if (ClassName == LidarPointCloudClass) return EMaterialUserType::LidarPointCloud;
		if (ClassName == WorldClass) return EMaterialUserType::Level;

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
		// On-disk data only: UE 4.27 finds in-memory assets by iterating every loaded object, which takes seconds
		// per call in a large project and is repeated for every referencing package.
		AssetRegistry.GetAssetsByPackageName(ReferencerPackage, PackageAssets, /*bIncludeOnlyOnDiskAssets*/ true);

		FMaterialUser User;
		User.Type = ClassifyReferencer(AssetRegistry, ReferencerPackage, PackageAssets);
		User.Path = PackageAssets.Num() > 0
			? PackageAssets[0].ObjectPath.ToString()
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
		/** Usage flag needed, or MATUSAGE_MAX when none is needed or the component or emitter type is not recognised. */
		EMaterialUsage Usage = MATUSAGE_MAX;

		/** Materials drawn this way. Empty means the use applies to every material of the user. */
		TArray<FSoftObjectPath> Materials;

		/** Whether the report recognises the component or emitter type, so Usage is known. */
		bool bRecognised = true;
	};

	/** Adds a use of Usage for the valid entries of Materials to Uses. bRecognised is false for unknown component types. */
	static void AddMaterialUse(
		TArray<FMaterialUse>& Uses,
		const EMaterialUsage Usage,
		const TArray<UMaterialInterface*>& Materials,
		const bool bRecognised = true)
	{
		FMaterialUse& Use = Uses.AddDefaulted_GetRef();
		Use.Usage = Usage;
		Use.bRecognised = bRecognised;
		for (const UMaterialInterface* Material : Materials)
		{
			if (Material != nullptr) Use.Materials.AddUnique(FSoftObjectPath(Material));
		}
	}

	/**
	 * Finds the usage flag a component needs for the materials it draws.
	 * Output: OutUsage is that flag, or MATUSAGE_MAX when the component needs none: UE 4.27 has no static mesh
	 * usage flag, so a plain static mesh component needs none.
	 * Returns: False for component types this report does not recognise.
	 */
	static bool TryGetComponentUsage(const UPrimitiveComponent& Component, EMaterialUsage& OutUsage)
	{
		OutUsage = MATUSAGE_MAX;
		// Subclasses are checked before UStaticMeshComponent because they derive from it.
		if (Component.IsA<USplineMeshComponent>()) OutUsage = MATUSAGE_SplineMesh;
		else if (Component.IsA<UInstancedStaticMeshComponent>()) OutUsage = MATUSAGE_InstancedStaticMeshes;
		else if (Component.IsA<UStaticMeshComponent>()) return true;
		else if (Component.IsA<USkeletalMeshComponent>()) OutUsage = MATUSAGE_SkeletalMesh;
		if (OutUsage != MATUSAGE_MAX) return true;

		// Compared by name, so the GeometryCollectionEngine module is not needed as a dependency.
		static const FName GeometryCollectionComponentClass(TEXT("GeometryCollectionComponent"));
		for (const UClass* Class = Component.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == GeometryCollectionComponentClass)
			{
				OutUsage = MATUSAGE_GeometryCollections;
				return true;
			}
		}
		return false;
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
			EMaterialUsage Usage = MATUSAGE_MAX;
			const bool bRecognised = TryGetComponentUsage(*PrimitiveComponent, Usage);
			AddMaterialUse(Uses, Usage, UsedMaterials, bRecognised);
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
		return Uses;
	}

	/**
	 * Reads the package header at PackageName, without creating any of its objects, and checks whether one of its
	 * objects has the class named ClassName.
	 * Input: The package must have no objects or open header in memory, so the header is not shared with a loaded
	 * or loading package.
	 * Output: bOutFound says whether such an object exists. The header is closed again; the empty package object
	 * it leaves is unloaded with the batch's other packages.
	 * Returns: False when the header cannot be read.
	 */
	static bool TryFindPackageObjectOfClass(const FName PackageName, const FName ClassName, bool& bOutFound)
	{
		if (!FPackageName::DoesPackageExist(PackageName.ToString())) return false;
		FLinkerLoad* Linker = GetPackageLinker(nullptr, *PackageName.ToString(), LOAD_NoVerify | LOAD_Quiet | LOAD_NoWarn, nullptr, nullptr);
		if (Linker == nullptr) return false;

		bOutFound = Linker->ExportMap.ContainsByPredicate([Linker, ClassName](const FObjectExport& Export)
		{
			return !Export.ClassIndex.IsNull() && Linker->ImpExp(Export.ClassIndex).ObjectName == ClassName;
		});
		ResetLoaders(Linker->LinkerRoot);
		return true;
	}

	/**
	 * Finds the same uses as FindSkeletalMeshUses for a skeletal mesh that is not loaded, without loading it, so
	 * its render data, skeleton, physics asset, materials and textures stay on disk. Morph targets come from the
	 * Asset Registry tag and clothing from its package header.
	 * Clothing assets count even when no mesh section uses them, unlike USkeletalMesh::HasActiveClothingAssets.
	 * Output: Fills OutUses when it returns true.
	 * Returns: False when the asset is not an unloaded skeletal mesh or its data cannot be read this way, so the
	 * caller loads it instead.
	 */
	static bool TryFindUnloadedSkeletalMeshUses(IAssetRegistry& AssetRegistry, const FSoftObjectPath& UserPath, TArray<FMaterialUse>& OutUses)
	{
		const FAssetData UserData = AssetRegistry.GetAssetByObjectPath(FName(*UserPath.ToString()));
		if (!UserData.IsValid() || UserData.AssetClass != USkeletalMesh::StaticClass()->GetFName()) return false;
		// Packages saved before the tag existed, or already in memory, are left to the loading path.
		int32 MorphTargetCount = 0;
		if (!UserData.GetTagValue(TEXT("MorphTargets"), MorphTargetCount)) return false;
		if (IsAsyncLoading()) return false;
		if (UPackage* ExistingPackage = FindPackage(nullptr, *UserData.PackageName.ToString()))
		{
			// An empty package left by an earlier header read can be read again; a package with objects cannot.
			bool bHasObjects = false;
			ForEachObjectWithPackage(ExistingPackage, [&bHasObjects](UObject*)
			{
				bHasObjects = true;
				return false;
			}, false);
			// UE 4.27's UPackage has no GetLinker; the open header is found through FLinkerLoad instead.
			if (bHasObjects || FLinkerLoad::FindExistingLinkerForPackage(ExistingPackage) != nullptr) return false;
		}

		bool bHasClothing = false;
		if (!TryFindPackageObjectOfClass(UserData.PackageName, TEXT("ClothingAssetCommon"), bHasClothing)) return false;

		OutUses.Reset();
		if (MorphTargetCount > 0) OutUses.Add({ MATUSAGE_MorphTargets, {} });
		if (bHasClothing) OutUses.Add({ MATUSAGE_Clothing, {} });
		return true;
	}

	/** Gathers the Material Analytics report over several editor ticks and saves it when done. */
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
			TickerHandle = FTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FMaterialAnalyticsRunner::Tick));
		}

		/** Stops gathering and throws away unfinished results. */
		void Stop()
		{
			AbortRun();
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
			/** Loads the current master's users for Vertex Factory Users within a per-tick time budget. */
			ResolveUsers,
			/** Reads the current master's instances for Shader Permutations within a per-tick time budget. */
			ReadPermutations,
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
			UserResolve = FUserResolveState();
			PermutationRead = FPermutationReadState();
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

		/** Stops ticking, throws away unfinished results, and unloads the packages the run loaded. */
		void AbortRun()
		{
			if (TickerHandle.IsValid())
			{
				FTicker::GetCoreTicker().RemoveTicker(TickerHandle);
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
				UMaterialInstanceConstant::StaticClass()->GetFName(),
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
				const FSoftObjectPath CurrentPath = PathsToVisit.Pop(/*bAllowShrinking*/ false);
				const TArray<FAssetData>* ChildInstances = ChildInstancesByParent.Find(CurrentPath);
				if (ChildInstances == nullptr) continue;

				for (const FAssetData& ChildInstance : *ChildInstances)
				{
					// The visited check guards against broken data that loops back on itself.
					bool bAlreadyVisited = false;
					VisitedInstances.Add(ChildInstance.ToSoftObjectPath(), &bAlreadyVisited);
					if (bAlreadyVisited) continue;
					if (Settings.bIgnoreUnreferencedAssets &&
						!Core::HasRelevantReferencer(AssetRegistry, ChildInstance, Settings.bIgnoreDeveloperReferences)) continue;

					CountedInstances.Add(ChildInstance);
					PathsToVisit.Add(ChildInstance.ToSoftObjectPath());
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
					if (Settings.bIgnoreDeveloperReferences && Core::IsDeveloperPackage(Referencer)) continue;
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
			Filter.ClassNames.Add(UMaterial::StaticClass()->GetFName());
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
		 * Builds the shader map key of Instance: its effective static parameters with every bOverride cleared, as
		 * FMaterialShaderMapId::UpdateFromParameterSet does, so an inherited value and an overridden equal value match.
		 */
		static FStaticPermutationKey MakePermutationKey(UMaterialInstance& Instance, const uint64 BasePropertyOverridesHash)
		{
			FStaticPermutationKey Key;
			Instance.GetStaticParameterValues(Key.StaticParameters);
			for (FStaticSwitchParameter& SwitchParameter : Key.StaticParameters.StaticSwitchParameters)
			{
				SwitchParameter.bOverride = false;
			}
			for (FStaticComponentMaskParameter& MaskParameter : Key.StaticParameters.StaticComponentMaskParameters)
			{
				MaskParameter.bOverride = false;
			}
			Key.BasePropertyOverridesHash = BasePropertyOverridesHash;
			return Key;
		}

		/**
		 * Builds the shader map key of a master material: its static switches and component masks at their default
		 * values, with bOverride cleared. UE 4.27's UMaterial has no GetStaticParameterValues, so they are read one by
		 * one. The master's base properties are its own, so its base property hash is 0, as for instances that change none.
		 */
		static FStaticPermutationKey MakeMasterPermutationKey(const UMaterial& Material)
		{
			FStaticPermutationKey Key;
			TArray<FMaterialParameterInfo> ParameterInfos;
			TArray<FGuid> ParameterGuids;
			Material.GetAllStaticSwitchParameterInfo(ParameterInfos, ParameterGuids);
			for (const FMaterialParameterInfo& SwitchInfo : ParameterInfos)
			{
				bool bValue = false;
				FGuid ExpressionGuid;
				Material.GetStaticSwitchParameterDefaultValue(FHashedMaterialParameterInfo(SwitchInfo), bValue, ExpressionGuid);
				Key.StaticParameters.StaticSwitchParameters.Emplace(SwitchInfo, bValue, false, ExpressionGuid);
			}

			ParameterInfos.Reset();
			ParameterGuids.Reset();
			Material.GetAllStaticComponentMaskParameterInfo(ParameterInfos, ParameterGuids);
			for (const FMaterialParameterInfo& MaskInfo : ParameterInfos)
			{
				bool bR = false, bG = false, bB = false, bA = false;
				FGuid ExpressionGuid;
				Material.GetStaticComponentMaskParameterDefaultValue(FHashedMaterialParameterInfo(MaskInfo), bR, bG, bB, bA, ExpressionGuid);
				Key.StaticParameters.StaticComponentMaskParameters.Emplace(MaskInfo, bR, bG, bB, bA, false, ExpressionGuid);
			}
			return Key;
		}

		/** Returns mask channels as report text, such as "R_B_" for red and blue. */
		static FString FormatComponentMask(const bool bR, const bool bG, const bool bB, const bool bA)
		{
			return FString::Printf(TEXT("%s%s%s%s"), bR ? TEXT("R") : TEXT("_"), bG ? TEXT("G") : TEXT("_"), bB ? TEXT("B") : TEXT("_"), bA ? TEXT("A") : TEXT("_"));
		}

		/**
		 * Lists the master's static switches, static component masks and overridable base properties with their
		 * defaults, ready for instance overrides to be counted against them.
		 * Output: Fills Item.PermutationParameters.
		 */
		static void AddPermutationParameters(const UMaterial& MasterMaterial, FMaterialAssetData& Item)
		{
			TArray<FMaterialParameterInfo> ParameterInfos;
			TArray<FGuid> ParameterGuids;
			MasterMaterial.GetAllStaticSwitchParameterInfo(ParameterInfos, ParameterGuids);
			for (const FMaterialParameterInfo& SwitchInfo : ParameterInfos)
			{
				FPermutationParameterSummary& Summary = Item.PermutationParameters.AddDefaulted_GetRef();
				Summary.Kind = EPermutationParameterKind::StaticSwitch;
				Summary.ParameterInfo = SwitchInfo;
				bool bDefaultValue = false;
				FGuid DefaultExpressionGuid;
				MasterMaterial.GetStaticSwitchParameterDefaultValue(FHashedMaterialParameterInfo(SwitchInfo), bDefaultValue, DefaultExpressionGuid);
				Summary.DefaultValue = bDefaultValue ? TEXT("True") : TEXT("False");
			}

			ParameterInfos.Reset();
			ParameterGuids.Reset();
			MasterMaterial.GetAllStaticComponentMaskParameterInfo(ParameterInfos, ParameterGuids);
			for (const FMaterialParameterInfo& MaskInfo : ParameterInfos)
			{
				FPermutationParameterSummary& Summary = Item.PermutationParameters.AddDefaulted_GetRef();
				Summary.Kind = EPermutationParameterKind::ComponentMask;
				Summary.ParameterInfo = MaskInfo;
				bool bR = false, bG = false, bB = false, bA = false;
				FGuid DefaultExpressionGuid;
				MasterMaterial.GetStaticComponentMaskParameterDefaultValue(FHashedMaterialParameterInfo(MaskInfo), bR, bG, bB, bA, DefaultExpressionGuid);
				Summary.DefaultValue = FormatComponentMask(bR, bG, bB, bA);
			}

			for (const FBasePropertyCheck& BaseProperty : BasePropertyChecks)
			{
				FPermutationParameterSummary& Summary = Item.PermutationParameters.AddDefaulted_GetRef();
				Summary.Kind = EPermutationParameterKind::BaseProperty;
				Summary.ParameterInfo.Name = BaseProperty.Name;
				Summary.DefaultValue = BaseProperty.ValueText(MasterMaterial);
			}
		}

		/**
		 * Compares the instance's static parameters and base properties with the master and its parent. Overrides
		 * inherited from a parent instance are counted on that parent, so each override is counted once.
		 * Input: Instance must be loaded and have a parent. InstanceKey is the instance's shader map key, which holds
		 * the static parameter values it ends up with.
		 * Output: Increases ChangedCount for own overrides that differ from the parent's value, including ones that
		 * restore the master's value, and NoOpCount for own overrides that repeat the parent's value. Adds a cell to
		 * Row for each parameter the instance overrides, or inherits with a value different from the master's.
		 */
		static void ReadInstancePermutationCells(
			const UMaterial& MasterMaterial,
			const UMaterialInstance& Instance,
			const FStaticPermutationKey& InstanceKey,
			FMaterialAssetData& Item,
			FInstancePermutationRow& Row)
		{
			const UMaterialInterface& Parent = *Instance.Parent;
			const FStaticParameterSet& OwnStaticParameters = Instance.GetStaticParameters();
			auto FindParameterIndex = [&Item](const EPermutationParameterKind Kind, const FMaterialParameterInfo& ParameterInfo)
			{
				return Item.PermutationParameters.IndexOfByPredicate([Kind, &ParameterInfo](const FPermutationParameterSummary& Candidate)
				{
					return Candidate.Kind == Kind && Candidate.ParameterInfo == ParameterInfo;
				});
			};
			auto AddCell = [&Item, &Row](const int32 ParameterIndex, const FString& Value, const bool bOwnOverride, const bool bDiffersFromMaster, const bool bSameAsParent)
			{
				if (ParameterIndex == INDEX_NONE) return;
				FPermutationParameterSummary& Summary = Item.PermutationParameters[ParameterIndex];
				if (bOwnOverride && !bSameAsParent) ++Summary.ChangedCount;
				if (bOwnOverride && bSameAsParent) ++Summary.NoOpCount;
				// Inherited parameters with the master's value need no cell; the report shows them with the master's default.
				if (!bOwnOverride && !bDiffersFromMaster) return;
				FInstancePermutationCell& Cell = Row.Cells.AddDefaulted_GetRef();
				Cell.ParameterIndex = ParameterIndex;
				Cell.Value = Value;
				Cell.bOwnOverride = bOwnOverride;
				Cell.bSameAsParent = bOwnOverride && bSameAsParent;
			};

			for (const FStaticSwitchParameter& SwitchParameter : InstanceKey.StaticParameters.StaticSwitchParameters)
			{
				const int32 ParameterIndex = FindParameterIndex(EPermutationParameterKind::StaticSwitch, SwitchParameter.ParameterInfo);
				const FString ValueText = SwitchParameter.Value ? TEXT("True") : TEXT("False");
				const bool bOwnOverride = OwnStaticParameters.StaticSwitchParameters.ContainsByPredicate([&SwitchParameter](const FStaticSwitchParameter& OwnParameter)
				{
					return OwnParameter.bOverride && OwnParameter.ParameterInfo == SwitchParameter.ParameterInfo;
				});
				bool bParentValue = false;
				FGuid ParentExpressionGuid;
				const bool bSameAsParent =
					Parent.GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(SwitchParameter.ParameterInfo), bParentValue, ParentExpressionGuid) &&
					bParentValue == SwitchParameter.Value;
				const bool bDiffersFromMaster = ParameterIndex != INDEX_NONE && Item.PermutationParameters[ParameterIndex].DefaultValue != ValueText;
				AddCell(ParameterIndex, ValueText, bOwnOverride, bDiffersFromMaster, bSameAsParent);
			}
			for (const FStaticComponentMaskParameter& MaskParameter : InstanceKey.StaticParameters.StaticComponentMaskParameters)
			{
				const int32 ParameterIndex = FindParameterIndex(EPermutationParameterKind::ComponentMask, MaskParameter.ParameterInfo);
				const FString MaskText = FormatComponentMask(MaskParameter.R, MaskParameter.G, MaskParameter.B, MaskParameter.A);
				const bool bOwnOverride = OwnStaticParameters.StaticComponentMaskParameters.ContainsByPredicate([&MaskParameter](const FStaticComponentMaskParameter& OwnParameter)
				{
					return OwnParameter.bOverride && OwnParameter.ParameterInfo == MaskParameter.ParameterInfo;
				});
				bool bParentR = false, bParentG = false, bParentB = false, bParentA = false;
				FGuid ParentExpressionGuid;
				const bool bSameAsParent =
					Parent.GetStaticComponentMaskParameterValue(FHashedMaterialParameterInfo(MaskParameter.ParameterInfo), bParentR, bParentG, bParentB, bParentA, ParentExpressionGuid) &&
					FormatComponentMask(bParentR, bParentG, bParentB, bParentA) == MaskText;
				const bool bDiffersFromMaster = ParameterIndex != INDEX_NONE && Item.PermutationParameters[ParameterIndex].DefaultValue != MaskText;
				AddCell(ParameterIndex, MaskText, bOwnOverride, bDiffersFromMaster, bSameAsParent);
			}
			for (const FBasePropertyCheck& BaseProperty : BasePropertyChecks)
			{
				FMaterialParameterInfo PropertyInfo;
				PropertyInfo.Name = BaseProperty.Name;
				const bool bDiffersFromMaster = !BaseProperty.HasSameValue(Instance, MasterMaterial);
				AddCell(
					FindParameterIndex(EPermutationParameterKind::BaseProperty, PropertyInfo),
					BaseProperty.ValueText(Instance),
					BaseProperty.IsOverridden(Instance.BasePropertyOverrides),
					bDiffersFromMaster,
					BaseProperty.HasSameValue(Instance, Parent));
			}
		}

		/**
		 * Progress of reading one master's static permutations. Its instances are read over several ticks, because
		 * loading an instance whose shaders are not cached starts compiling them, which can take seconds each.
		 */
		struct FPermutationReadState
		{
			/** Index in AssetItems of the master being read, or INDEX_NONE when no master is being read. */
			int32 ItemIndex = INDEX_NONE;
			/** The loaded master. */
			TWeakObjectPtr<UMaterial> MasterMaterial;
			/** Shader platform the layout shader counts are for. */
			EShaderPlatform ShaderPlatform = SP_NumPlatforms;
			/** Shader map key of the master. */
			FStaticPermutationKey MasterKey;
			/** Keys of the instances counted so far that compile their own, unique shader map. */
			TArray<FStaticPermutationKey> PermutationKeys;
			/** Index in the master's Instances of the next instance to read. */
			int32 NextInstanceIndex = 0;
			/** Layout shaders counted so far for the master and its unique permutations. */
			int32 LayoutShaderCount = 0;
		};

		/**
		 * Loads the master of Item to start reading its static permutations on the editor's shader platform: counts
		 * the master's layout shaders for each of its quality shader maps, and lists its static parameters and base
		 * properties.
		 * Output: On success, OutState is ready for ReadInstancePermutation with ItemIndex as the master's report row.
		 * OutState.ItemIndex stays INDEX_NONE when the master cannot be loaded.
		 * Returns: The number of materials loaded from disk.
		 */
		static int32 BeginStaticPermutations(const FAssetData& MasterData, FMaterialAssetData& Item, const int32 ItemIndex, FPermutationReadState& OutState)
		{
			OutState = FPermutationReadState();
			const bool bWasMasterLoaded = MasterData.IsAssetLoaded();
			UMaterial* MasterMaterial = Cast<UMaterial>(MasterData.GetAsset());
			if (MasterMaterial == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material %s to read its static permutations."), *MasterData.ObjectPath.ToString());
				return 0;
			}

			OutState.ItemIndex = ItemIndex;
			OutState.MasterMaterial = MasterMaterial;
			OutState.ShaderPlatform = GMaxRHIShaderPlatform;
			OutState.LayoutShaderCount = CountLayoutShaders(*MasterMaterial, OutState.ShaderPlatform, CountQualityShaderMaps(*MasterMaterial, OutState.ShaderPlatform));
			AddPermutationParameters(*MasterMaterial, Item);
			OutState.MasterKey = MakeMasterPermutationKey(*MasterMaterial);
			return bWasMasterLoaded ? 0 : 1;
		}

		/**
		 * Loads one counted instance and reads its static permutation. An instance that overrides static parameters
		 * or base properties compiles its own shader map; when its shader map key matches the master's or an earlier
		 * instance's, it shares that shader map, so layout shaders are only counted once per unique permutation,
		 * repeated for each of the instance's quality shader maps.
		 * Output: Adds the instance's row and, for a static permutation instance, its path and layout shaders.
		 * Returns: 1 when this call loaded the instance from disk, otherwise 0.
		 */
		static int32 ReadInstancePermutation(const UMaterial& MasterMaterial, const FAssetData& InstanceData, FMaterialAssetData& Item, FPermutationReadState& State)
		{
			const bool bWasAssetLoaded = InstanceData.IsAssetLoaded();
			UMaterialInstance* Instance = Cast<UMaterialInstance>(InstanceData.GetAsset());
			if (Instance == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material instance %s to read its static permutation."), *InstanceData.ObjectPath.ToString());
				return 0;
			}
			const int32 LoadedAssetCount = bWasAssetLoaded ? 0 : 1;

			FStaticPermutationKey Key = MakePermutationKey(*Instance, GetBasePropertyOverridesHash(*Instance));
			FInstancePermutationRow& Row = Item.InstancePermutationRows.AddDefaulted_GetRef();
			Row.Path = InstanceData.ObjectPath.ToString();
			if (Instance->Parent != nullptr) ReadInstancePermutationCells(MasterMaterial, *Instance, Key, Item, Row);

			if (!Instance->bHasStaticPermutationResource) return LoadedAssetCount;

			Item.StaticPermutationInstancePaths.Add(InstanceData.ObjectPath.ToString());
			// Instances whose key matches the master's or an earlier instance's share that shader map.
			const bool bAlreadySeen = State.MasterKey.CompilesSameShaders(Key) ||
				State.PermutationKeys.ContainsByPredicate([&Key](const FStaticPermutationKey& SeenKey)
				{
					return SeenKey.CompilesSameShaders(Key);
				});
			if (bAlreadySeen) return LoadedAssetCount;

			State.PermutationKeys.Add(MoveTemp(Key));
			// The layout only covers the instance's active quality level, so it is repeated for each quality shader map.
			State.LayoutShaderCount += CountLayoutShaders(*Instance, State.ShaderPlatform, CountQualityShaderMaps(*Instance, State.ShaderPlatform));
			return LoadedAssetCount;
		}

		/**
		 * Stores the finished static permutation results of Item.
		 * Output: Sorts Item's instance rows and permutation paths, fills LayoutShaderCount, and sets
		 * Item.bShaderPermutationsRead. Resets State so no master is being read.
		 */
		static void FinishStaticPermutations(FMaterialAssetData& Item, FPermutationReadState& State)
		{
			Item.InstancePermutationRows.Sort([](const FInstancePermutationRow& Left, const FInstancePermutationRow& Right)
			{
				return Left.Path < Right.Path;
			});
			Item.StaticPermutationInstancePaths.Sort();
			Item.LayoutShaderCount = State.LayoutShaderCount;
			Item.bShaderPermutationsRead = true;
			State = FPermutationReadState();
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
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics could not load material %s to read its usage flags."), *AssetData.ObjectPath.ToString());
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
		 * Returns the material uses of the user at UserPath, reading it the first time it is needed in this run.
		 * Skeletal meshes that are not loaded are read without loading them; other users are loaded.
		 * Output: Increases LoadedAssetCount when this call loaded the user from disk.
		 */
		const TArray<FMaterialUse>& GetUserUses(const FString& UserPath, int32& LoadedAssetCount)
		{
			if (const TArray<FMaterialUse>* CachedUses = UsesByUserPath.Find(UserPath)) return *CachedUses;

			const FSoftObjectPath UserObjectPath(UserPath);
			UObject* User = UserObjectPath.ResolveObject();
			if (User == nullptr)
			{
				TArray<FMaterialUse> SkeletalMeshUses;
				if (TryFindUnloadedSkeletalMeshUses(IAssetRegistry::GetChecked(), UserObjectPath, SkeletalMeshUses))
				{
					return UsesByUserPath.Add(UserPath, MoveTemp(SkeletalMeshUses));
				}
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

		/** A Blueprint, Cascade particle system or skeletal mesh user to read for Vertex Factory Users. */
		struct FUserToResolve
		{
			/** Registry user type the user is listed under. */
			EMaterialUserType Type = EMaterialUserType::Other;
			/** Object path of the user. */
			FString Path;
		};

		/**
		 * Progress of resolving one master's users by loading them. Users are read over several ticks, because
		 * loading a Blueprint can pull in many other assets and take seconds.
		 */
		struct FUserResolveState
		{
			/** Index in AssetItems of the master whose users are read, or INDEX_NONE when none is being read. */
			int32 ItemIndex = INDEX_NONE;
			/** The master and its counted instances, so only uses of these materials are counted. */
			TSet<FSoftObjectPath> TreeMaterials;
			/** Users to read, in order. */
			TArray<FUserToResolve> Users;
			/** Index in Users of the next user to read. */
			int32 NextUserIndex = 0;
			/** Blueprint and particle system users whose uses of the master were all recognised. */
			TArray<FUserToResolve> FullyResolvedUsers;
		};

		/**
		 * Starts resolving the Blueprint, Cascade particle system and skeletal mesh users of Item, which ResolveUser
		 * then reads one at a time.
		 * Input: MaterialAndInstances is the master and its counted instances.
		 * Output: Sets UserResolve for Item, or leaves it unset when Item has no such users.
		 */
		void BeginResolvingUsers(FMaterialAssetData& Item, const int32 ItemIndex, const TArray<FAssetData>& MaterialAndInstances)
		{
			UserResolve = FUserResolveState();
			Item.ResolvedUserPathsByUsage.SetNum(MATUSAGE_MAX);
			const EMaterialUserType ResolvableTypes[] =
			{
				EMaterialUserType::Blueprint,
				EMaterialUserType::Particle,
				EMaterialUserType::SkeletalMesh
			};
			for (const EMaterialUserType UserType : ResolvableTypes)
			{
				for (const FString& UserPath : Item.UserPathsByType[static_cast<int32>(UserType)])
				{
					UserResolve.Users.Add({ UserType, UserPath });
				}
			}
			if (UserResolve.Users.Num() == 0) return;

			UserResolve.ItemIndex = ItemIndex;
			for (const FAssetData& MaterialOrInstance : MaterialAndInstances)
			{
				UserResolve.TreeMaterials.Add(MaterialOrInstance.ToSoftObjectPath());
			}
		}

		/**
		 * Reads one user of the master in UserResolve and files it under the usage flags it needs. Blueprints and
		 * particle systems are loaded; skeletal meshes are read without loading when possible (see GetUserUses).
		 * Output: Adds the user to Item's resolved users, and to UserResolve.FullyResolvedUsers when it is a
		 * Blueprint or particle system whose uses of the master are all recognised.
		 * Returns: The number of users loaded from disk.
		 */
		int32 ResolveUser(const FUserToResolve& User, FMaterialAssetData& Item)
		{
			int32 LoadedAssetCount = 0;
			TSet<EMaterialUsage> NeededUsages;
			bool bHasUnrecognisedUse = false;
			for (const FMaterialUse& Use : GetUserUses(User.Path, LoadedAssetCount))
			{
				const TSet<FSoftObjectPath>& TreeMaterials = UserResolve.TreeMaterials;
				const bool bUsesThisMaterial = Use.Materials.Num() == 0 ||
					Use.Materials.ContainsByPredicate([&TreeMaterials](const FSoftObjectPath& Material)
					{
						return TreeMaterials.Contains(Material);
					});
				if (!bUsesThisMaterial) continue;
				if (!Use.bRecognised) bHasUnrecognisedUse = true;
				else if (Use.Usage != MATUSAGE_MAX) NeededUsages.Add(Use.Usage);
			}

			for (const EMaterialUsage Usage : NeededUsages)
			{
				Item.ResolvedUserPathsByUsage[Usage].AddUnique(User.Path);
			}
			const bool bFullyResolved = NeededUsages.Num() > 0 && !bHasUnrecognisedUse;
			if (User.Type != EMaterialUserType::SkeletalMesh && bFullyResolved) UserResolve.FullyResolvedUsers.Add(User);
			return LoadedAssetCount;
		}

		/**
		 * Stores the resolved users of the master in UserResolve: Blueprints and particle systems whose uses were
		 * all recognised are removed from their broad group; skeletal meshes stay under SkeletalMesh.
		 * Output: Sorts Item's resolved user lists and resets UserResolve.
		 */
		void FinishResolvingUsers(FMaterialAssetData& Item)
		{
			for (const FUserToResolve& User : UserResolve.FullyResolvedUsers)
			{
				Item.UserPathsByType[static_cast<int32>(User.Type)].Remove(User.Path);
			}
			for (TArray<FString>& ResolvedUserPaths : Item.ResolvedUserPathsByUsage)
			{
				ResolvedUserPaths.Sort();
			}
			UserResolve = FUserResolveState();
		}

		/**
		 * Fills the report row of one master material, loading it when the settings ask for it. Users and instances
		 * that need loading are read on later ticks: with Vertex Factory Users on, UserResolve is set for the
		 * ResolveUsers step; with Shader Permutations on, PermutationRead is set for the ReadPermutations step.
		 * Returns: The number of assets this call loaded from disk.
		 */
		int32 ProcessMaterial(IAssetRegistry& AssetRegistry, const UAssetAnalyticsSettings& Settings, const FAssetData& AssetData)
		{
			if (Settings.bIgnoreUnreferencedAssets &&
				!Core::HasRelevantReferencer(AssetRegistry, AssetData, Settings.bIgnoreDeveloperReferences)) return 0;

			int32 LoadedAssetCount = 0;
			const int32 ItemIndex = AssetItems.Num();
			FMaterialAssetData& Item = AssetItems.Emplace_GetRef(AssetData);
			if (Settings.bMaterialPackageDiskSize)
			{
				if (const FAssetPackageData* PackageData = AssetRegistry.GetAssetPackageData(AssetData.PackageName))
				{
					Item.PackageDiskSize = PackageData->DiskSize;
				}
			}
			if (NeedsInstanceTree(Settings))
			{
				Item.Instances = GatherCountedInstances(AssetRegistry, AssetData.ToSoftObjectPath(), Settings);
				if (Settings.bMaterialUserTypeCounts)
				{
					TArray<FAssetData> MaterialAndInstances = Item.Instances;
					MaterialAndInstances.Add(AssetData);
					GatherUsersByType(AssetRegistry, MaterialAndInstances, Settings, Item);
					if (Settings.bMaterialResolveUsersByLoading) BeginResolvingUsers(Item, ItemIndex, MaterialAndInstances);
				}
			}
			if (Settings.bMaterialUsageFlags && ReadUsageFlags(AssetData, Item)) ++LoadedAssetCount;
			if (Settings.bMaterialShaderPermutations) LoadedAssetCount += BeginStaticPermutations(AssetData, Item, ItemIndex, PermutationRead);
			return LoadedAssetCount;
		}

		/**
		 * Ends the batch when it is full or no materials are left: when it loaded assets, the next tick unloads them.
		 * Returns: True when the unload step was started; false when the report keeps processing materials.
		 */
		bool TryFinishBatch(const UAssetAnalyticsSettings& Settings)
		{
			const int32 MaterialsPerBatch = FMath::Max(Settings.AssetsPerTick, 1);
			const bool bBatchFinished = MaterialsInBatch >= MaterialsPerBatch || NextAssetIndex >= PendingAssets.Num();
			if (!bBatchFinished) return false;
			if (BatchLoadedAssetCount == 0)
			{
				MaterialsInBatch = 0;
				return false;
			}

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

		/** Tells the user which master's instances are being read and how far along it is. */
		void UpdatePermutationNotification() const
		{
			const FMaterialAssetData& Item = AssetItems[PermutationRead.ItemIndex];
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("ReadingStaticPermutations", "Material Analytics - {0} / {1}\nReading static permutations of {2}: instance {3} / {4}\nInstances with uncached shaders start compiling when they load."),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::FromName(Item.AssetData.AssetName),
				FText::AsNumber(PermutationRead.NextInstanceIndex),
				FText::AsNumber(Item.Instances.Num())));
		}

		/**
		 * Reads the current master's instances until the tick time budget is used, at least one per tick, so the
		 * editor keeps repainting and Cancel keeps working while instances load.
		 * Once all are read, stores the results and ends the batch or goes back to processing materials.
		 * Returns: True, since the report is still running.
		 */
		bool ReadPermutationsForOneTick(const UAssetAnalyticsSettings& Settings)
		{
			const double TickStartSeconds = FPlatformTime::Seconds();
			FMaterialAssetData& Item = AssetItems[PermutationRead.ItemIndex];
			const UMaterial* MasterMaterial = PermutationRead.MasterMaterial.Get();
			while (MasterMaterial != nullptr && PermutationRead.NextInstanceIndex < Item.Instances.Num())
			{
				const FAssetData& InstanceData = Item.Instances[PermutationRead.NextInstanceIndex];
				BatchLoadedAssetCount += ReadInstancePermutation(*MasterMaterial, InstanceData, Item, PermutationRead);
				++PermutationRead.NextInstanceIndex;
				if (FPlatformTime::Seconds() - TickStartSeconds >= TickTimeBudgetSeconds) break;
			}
			if (MasterMaterial != nullptr && PermutationRead.NextInstanceIndex < Item.Instances.Num())
			{
				if (FPlatformTime::Seconds() - MaterialStepStartSeconds >= SlowStepMessageDelaySeconds) UpdatePermutationNotification();
				return true;
			}

			if (MasterMaterial == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset Analytics stopped reading the static permutations of %s because it was unloaded."), *Item.AssetData.ObjectPath.ToString());
			}
			FinishStaticPermutations(Item, PermutationRead);
			if (StartNextMaterialStep(Settings)) return true;
			// The next ProcessMaterials tick completes the report when this was the last material.
			UpdateProgressNotification();
			return true;
		}

		/** Tells the user which master's users are being loaded and how far along it is. */
		void UpdateUserResolveNotification() const
		{
			const FMaterialAssetData& Item = AssetItems[UserResolve.ItemIndex];
			ProgressNotification.SetText(FText::Format(
				LOCTEXT("ResolvingMaterialUsers", "Material Analytics - {0} / {1}\nLoading users of {2}: {3} / {4}"),
				FText::AsNumber(NextAssetIndex),
				FText::AsNumber(PendingAssets.Num()),
				FText::FromName(Item.AssetData.AssetName),
				FText::AsNumber(UserResolve.NextUserIndex),
				FText::AsNumber(UserResolve.Users.Num())));
		}

		/**
		 * Reads the current master's users until the tick time budget is used, at least one per tick, so the editor
		 * keeps repainting and Cancel keeps working while Blueprints and particle systems load.
		 * Once all are read, stores the results and moves to the master's next step.
		 * Returns: True, since the report is still running.
		 */
		bool ResolveUsersForOneTick(const UAssetAnalyticsSettings& Settings)
		{
			const double TickStartSeconds = FPlatformTime::Seconds();
			FMaterialAssetData& Item = AssetItems[UserResolve.ItemIndex];
			while (UserResolve.NextUserIndex < UserResolve.Users.Num())
			{
				BatchLoadedAssetCount += ResolveUser(UserResolve.Users[UserResolve.NextUserIndex], Item);
				++UserResolve.NextUserIndex;
				if (FPlatformTime::Seconds() - TickStartSeconds >= TickTimeBudgetSeconds) break;
			}
			if (UserResolve.NextUserIndex < UserResolve.Users.Num())
			{
				if (FPlatformTime::Seconds() - MaterialStepStartSeconds >= SlowStepMessageDelaySeconds) UpdateUserResolveNotification();
				return true;
			}

			FinishResolvingUsers(Item);
			if (StartNextMaterialStep(Settings)) return true;
			// The next ProcessMaterials tick completes the report when this was the last material.
			UpdateProgressNotification();
			return true;
		}

		/**
		 * Moves on once a master's row, or one of its loading steps, is done: its users are loaded first, then its
		 * instances are read, then the batch ends or the report goes back to processing materials.
		 * Returns: True when the next tick runs another step; false when materials can keep being processed.
		 */
		bool StartNextMaterialStep(const UAssetAnalyticsSettings& Settings)
		{
			// The step's own message only replaces the progress text once the step is slow; see SlowStepMessageDelaySeconds.
			if (UserResolve.ItemIndex != INDEX_NONE)
			{
				Step = ERunnerStep::ResolveUsers;
				MaterialStepStartSeconds = FPlatformTime::Seconds();
				UpdateProgressNotification();
				return true;
			}
			if (PermutationRead.ItemIndex != INDEX_NONE)
			{
				Step = ERunnerStep::ReadPermutations;
				MaterialStepStartSeconds = FPlatformTime::Seconds();
				UpdateProgressNotification();
				return true;
			}
			Step = ERunnerStep::ProcessMaterials;
			return TryFinishBatch(Settings);
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

				// The master's users and instances are loaded on the following ticks; the batch ends after them.
				if (StartNextMaterialStep(Settings)) return true;
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

			case ERunnerStep::ResolveUsers:
				return ResolveUsersForOneTick(*Settings);

			case ERunnerStep::ReadPermutations:
				return ReadPermutationsForOneTick(*Settings);

			case ERunnerStep::UnloadPackages:
				PackageUnloader.UnloadReportPackages();
				UE_LOG(
					LogTemp,
					Log,
					TEXT("Material Analytics loaded and unloaded %d assets (masters, instances and users) for %d materials in %.2f s."),
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
				return Left.AssetData.ObjectPath.LexicalLess(Right.AssetData.ObjectPath);
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

		/** Returns the JSON name of a permutation parameter kind. */
		static const TCHAR* GetPermutationParameterKindName(const EPermutationParameterKind Kind)
		{
			switch (Kind)
			{
			case EPermutationParameterKind::StaticSwitch: return TEXT("StaticSwitch");
			case EPermutationParameterKind::ComponentMask: return TEXT("ComponentMask");
			case EPermutationParameterKind::BaseProperty: return TEXT("BaseProperty");
			}
			return TEXT("Unknown");
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
					Writer.WriteValue(Instance.ObjectPath.ToString());
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
				// Shaders of the master and one instance per unique static permutation, on the editor's shader platform
				// with the cooked game's permutations.
				Writer.WriteValue(TEXT("ShaderCount"), Item.LayoutShaderCount);

				// ChangedCount: counted instances whose own override differs from their parent.
				// NoOpCount: counted instances whose own override repeats the value inherited from their parent.
				Writer.WriteArrayStart(TEXT("PermutationParameters"));
				for (const FPermutationParameterSummary& Parameter : Item.PermutationParameters)
				{
					Writer.WriteObjectStart();
					Writer.WriteValue(TEXT("Kind"), GetPermutationParameterKindName(Parameter.Kind));
					Writer.WriteValue(TEXT("Name"), Parameter.ParameterInfo.Name.ToString());
					if (!Parameter.DefaultValue.IsEmpty()) Writer.WriteValue(TEXT("Default"), Parameter.DefaultValue);
					Writer.WriteValue(TEXT("ChangedCount"), Parameter.ChangedCount);
					Writer.WriteValue(TEXT("NoOpCount"), Parameter.NoOpCount);
					Writer.WriteObjectEnd();
				}
				Writer.WriteArrayEnd();
				Writer.WriteArrayStart(TEXT("StaticPermutationInstances"));
				for (const FString& InstancePath : Item.StaticPermutationInstancePaths)
				{
					Writer.WriteValue(InstancePath);
				}
				Writer.WriteArrayEnd();

				// Cells: parameters the instance overrides or inherits with a value different from the master's.
				// Parameter is an index into PermutationParameters. State is SameAsParent for an own override that
				// repeats the parent's value, Changed for an own override that differs from the parent's value, and
				// Inherited for a value different from the master's that comes from a parent instance.
				Writer.WriteArrayStart(TEXT("InstancePermutations"));
				for (const FInstancePermutationRow& Row : Item.InstancePermutationRows)
				{
					Writer.WriteObjectStart();
					Writer.WriteValue(TEXT("Path"), Row.Path);
					Writer.WriteArrayStart(TEXT("Cells"));
					for (const FInstancePermutationCell& Cell : Row.Cells)
					{
						const TCHAR* State = Cell.bSameAsParent ? TEXT("SameAsParent")
							: Cell.bOwnOverride ? TEXT("Changed")
							: TEXT("Inherited");
						Writer.WriteObjectStart();
						Writer.WriteValue(TEXT("Parameter"), Cell.ParameterIndex);
						Writer.WriteValue(TEXT("Value"), Cell.Value);
						Writer.WriteValue(TEXT("State"), State);
						Writer.WriteObjectEnd();
					}
					Writer.WriteArrayEnd();
					Writer.WriteObjectEnd();
				}
				Writer.WriteArrayEnd();
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
		 * Returns why the users of Entry are unknown, as the JSON UsersUnknownReason value.
		 * Input: Entry has no Users list in Item.
		 */
		static const TCHAR* GetUsersUnknownReason(const FVertexFactoryEntry& Entry, const FMaterialAssetData& Item)
		{
			// Users were not gathered at all, so even registry users are unknown.
			if (Item.UserPathsByType.Num() == 0) return TEXT("UsersNotGathered");
			switch (Entry.UnregisteredUserLookup)
			{
			case EUnregisteredUserLookup::Loading:
				return Item.ResolvedUserPathsByUsage.Num() > 0 ? TEXT("NoneFoundByLoading") : TEXT("LoadingOff");
			case EUnregisteredUserLookup::PlacedActors: return TEXT("PlacedActors");
			case EUnregisteredUserLookup::Niagara: return TEXT("Niagara");
			case EUnregisteredUserLookup::NotChecked: return TEXT("NotChecked");
			}
			return TEXT("NotChecked");
		}

		/**
		 * Writes one VertexFactories entry when it has something to show: users, or an enabled usage flag.
		 * Users is written only when the users are known, so an empty list means "enabled but unused". Otherwise
		 * UsersUnknownReason says why they are unknown.
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
			else
			{
				// UsersNotGathered: Vertex Factory Counts was off. LoadingOff: Vertex Factory Users was off.
				// NoneFoundByLoading: loaded users found none, but levels were not checked. PlacedActors, Niagara
				// and NotChecked: the report cannot find these users.
				Writer.WriteValue(TEXT("UsersUnknownReason"), GetUsersUnknownReason(Entry, Item));
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
			// The shader platform the counts are for. Index matches the Platform column of a cook's MaterialStats CSV.
			Writer->WriteObjectStart(TEXT("ShaderPlatform"));
			Writer->WriteValue(TEXT("Name"), LexToString(GMaxRHIShaderPlatform));
			Writer->WriteValue(TEXT("Index"), static_cast<int32>(GMaxRHIShaderPlatform));
			Writer->WriteObjectEnd();
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

		/** How long the notification is shown before the first blocking step runs. */
		static constexpr double NotificationDisplayDelaySeconds = 0.2;
		/** Longest time one tick spends processing materials before letting the editor repaint. */
		static constexpr double TickTimeBudgetSeconds = 0.05;
		/**
		 * How long a master's user or instance step runs before the notification switches to that step's message.
		 * Quick steps keep the normal progress text, so the message does not flicker too fast to read.
		 */
		static constexpr double SlowStepMessageDelaySeconds = 1.0;
		/** Report rows gathered so far, one per processed master material. */
		TArray<FMaterialAssetData> AssetItems;
		/** Master materials found in the configured folders, processed in order. */
		TArray<FAssetData> PendingAssets;
		/** Direct child instances for every material or instance that has any, keyed by the parent's object path. */
		TMap<FSoftObjectPath, TArray<FAssetData>> ChildInstancesByParent;
		/** Packages of every material instance in the instance tree, so they are not counted as users. */
		TSet<FName> InstancePackages;
		/** Type and report path of each referencing package classified so far in this run. */
		TMap<FName, FMaterialUser> UserByPackage;
		/** Material uses of each user loaded so far in this run, so every user is loaded only once. */
		TMap<FString, TArray<FMaterialUse>> UsesByUserPath;
		/** Unloads the packages each batch loaded. */
		Core::FReportPackageUnloader PackageUnloader;
		/** Ticker that runs Tick while a report is in progress; invalid when idle. */
		FDelegateHandle TickerHandle;
		/** Notification that shows the report progress and the Cancel button. */
		Core::FReportProgressNotification ProgressNotification;
		/** Index in PendingAssets of the next master material to process. */
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
		/** When the current master's user or instance step started, to tell whether it is slow. */
		double MaterialStepStartSeconds = 0.0;
		/** Users of the master whose users are being loaded, while Step is ResolveUsers. */
		FUserResolveState UserResolve;
		/** Static permutations of the master whose instances are being read, while Step is ReadPermutations. */
		FPermutationReadState PermutationRead;
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
