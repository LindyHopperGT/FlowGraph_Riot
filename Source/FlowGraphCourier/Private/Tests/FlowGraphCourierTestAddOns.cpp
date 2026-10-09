#include "FlowGraphCourierTestAddOns.h"

#include "Nodes/FlowPin.h"
#include "Types/FlowPinTypeNamesStandard.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

UFlowGraphCourierTestCachedPinAddOn::UFlowGraphCourierTestCachedPinAddOn()
{
	// Start with a constructor-built cache so Courier's property callback is required to update it.
	RebuildOutputPins();
}

#if WITH_EDITOR
// Exercise the callback contract used by cached AddOns: inspect the active member,
// normalize final values, rebuild the AddOn cache, then request parent reconstruction.
void UFlowGraphCourierTestCachedPinAddOn::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	Super::PostEditChangeChainProperty(PropertyChangedEvent);

	const FEditPropertyChain::TDoubleLinkedListNode* ActiveMemberNode = PropertyChangedEvent.PropertyChain.GetActiveMemberNode();
	if (!ActiveMemberNode)
	{
		return;
	}

	const FName ActiveMemberName = ActiveMemberNode->GetValue()->GetFName();
	if (ActiveMemberName == GET_MEMBER_NAME_CHECKED(ThisClass, Thresholds))
	{
		// Whole-array replacement is canonicalized once before rebuilding the cache.
		Thresholds.Sort();
		for (int32 ThresholdIndex = Thresholds.Num() - 1; ThresholdIndex > 0; --ThresholdIndex)
		{
			if (Thresholds[ThresholdIndex] == Thresholds[ThresholdIndex - 1])
			{
				Thresholds.RemoveAt(ThresholdIndex);
			}
		}
	}
	else if (ActiveMemberName == GET_MEMBER_NAME_CHECKED(ThisClass, Settings))
	{
		// Dotted writes target nested leaves, but the callback observes their shared top-level member.
		Settings.Values.Sort();
		for (int32 ValueIndex = Settings.Values.Num() - 1; ValueIndex > 0; --ValueIndex)
		{
			if (Settings.Values[ValueIndex] == Settings.Values[ValueIndex - 1])
			{
				Settings.Values.RemoveAt(ValueIndex);
			}
		}
	}
	else
	{
		return;
	}

	RebuildOutputPins();
	RequestReconstructionOnOwningFlowNode();
}
#endif

void UFlowGraphCourierTestCachedPinAddOn::RebuildOutputPins()
{
#if WITH_EDITORONLY_DATA
	// Regenerate the complete active output cache from both supported authored property shapes.
	OutputPins.Reset();
	for (const int32 Threshold : Thresholds)
	{
		OutputPins.Add(FFlowPin(FName(*FString::Printf(TEXT("th%d"), Threshold))));
	}
	for (const int32 Value : Settings.Values)
	{
		OutputPins.Add(FFlowPin(FName(*FString::Printf(TEXT("cfg%d"), Value + Settings.Offset))));
	}
#endif
}

UFlowGraphCourierTestInputPinAddOn::UFlowGraphCourierTestInputPinAddOn()
{
	// Use a typed data pin so direction tests also verify pin metadata survives aggregation.
	InputPins.Add(FFlowPin(FName(TEXT("NestedInput")),
		FFlowPinTypeName(FFlowPinTypeNamesStandard::PinTypeNameObject), UObject::StaticClass()));
}

UFlowGraphCourierTestOutputPinAddOn::UFlowGraphCourierTestOutputPinAddOn()
{
#if WITH_EDITORONLY_DATA
	// Keep output metadata distinct from the nested input fixture.
	OutputPins.Add(FFlowPin(FName(TEXT("NestedOutput")),
		FFlowPinTypeName(FFlowPinTypeNamesStandard::PinTypeNameObject), AActor::StaticClass()));
#endif
}
