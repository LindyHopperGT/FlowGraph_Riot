// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors
#pragma once

#include "Engine/DeveloperSettings.h"
#include "Types/FlowActorSpawnQueueMode.h"

#include "FlowGameFrameworkSettings.generated.h"

/** Configuration for FlowGameFramework actor spawning. */
UCLASS(Config = Game, defaultconfig, meta = (DisplayName = "Flow Game Framework"))
class FLOWGAMEFRAMEWORK_API UFlowGameFrameworkSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Default mode for new actor-spawn submissions. Use SubmitSpawnPass for aggregate completion. */
	UPROPERTY(Config, EditAnywhere, Category = "Actor Spawning")
	EFlowActorSpawnQueueMode DefaultActorSpawnQueueMode = EFlowActorSpawnQueueMode::Staggered;

	/** Quick staggered spawn successes allowed per world tick; fast failures do not count. */
	UPROPERTY(Config, EditAnywhere, Category = "Actor Spawning", meta = (ClampMin = "1"))
	int32 MaxActorSpawnSuccessesPerTick = 2;

	/** Maximum attempts in one automatic immediate flush; remaining records continue on later ticks. */
	UPROPERTY(Config, EditAnywhere, Category = "Actor Spawning", meta = (ClampMin = "1"))
	int32 MaxActorSpawnAttemptsPerFlush = 64;

#if WITH_EDITORONLY_DATA
	virtual FName GetCategoryName() const override { return FName("Flow Graph"); }
	virtual FText GetSectionText() const override { return INVTEXT("Actor Spawning"); }
#endif
};
