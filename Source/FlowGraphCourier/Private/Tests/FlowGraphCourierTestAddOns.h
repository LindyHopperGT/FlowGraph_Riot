#pragma once

#include "AddOns/FlowNodeAddOn.h"

#include "FlowGraphCourierTestAddOns.generated.h"

// Shared nested settings let importer tests prove that preview and execution both notify
// a cached-pin AddOn for dotted struct and array property paths.
USTRUCT()
struct FFlowGraphCourierTestCachedPinSettings
{
	GENERATED_BODY()

	// Values are normalized by the fixture callback before their generated pins are rebuilt.
	UPROPERTY(EditAnywhere, Category = "Courier Test")
	TArray<int32> Values;

	// Added to each value so two dotted writes must be visible to the same final pin rebuild.
	UPROPERTY(EditAnywhere, Category = "Courier Test")
	int32 Offset = 0;
};

// A no-op parent fixture for constructing a nested AddOn tree in direction tests.
UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphCourierTestCompositeAddOn : public UFlowNodeAddOn
{
	GENERATED_BODY()
};

// This AddOn caches output pins; only its editor property callback rebuilds them after Courier writes.
UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphCourierTestCachedPinAddOn : public UFlowNodeAddOn
{
	GENERATED_BODY()

public:
	UFlowGraphCourierTestCachedPinAddOn();

	// Top-level cache input used by the whole-array replacement regression.
	UPROPERTY(EditAnywhere, Category = "Courier Test")
	TArray<int32> Thresholds;

	// Nested cache inputs used to exercise dotted paths and multiple writes to one callback member.
	UPROPERTY(EditAnywhere, Category = "Courier Test")
	FFlowGraphCourierTestCachedPinSettings Settings;

#if WITH_EDITOR
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent) override;
#endif

private:
	// Rebuilds OutputPins from the authored properties after the callback normalizes them.
	void RebuildOutputPins();
};

// Supplies a typed nested input for directional aggregation coverage.
UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphCourierTestInputPinAddOn : public UFlowNodeAddOn
{
	GENERATED_BODY()

public:
	UFlowGraphCourierTestInputPinAddOn();
};

// Supplies a typed nested output for directional aggregation coverage.
UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphCourierTestOutputPinAddOn : public UFlowNodeAddOn
{
	GENERATED_BODY()

public:
	UFlowGraphCourierTestOutputPinAddOn();
};
