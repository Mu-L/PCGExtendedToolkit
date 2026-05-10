// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Channels/PCGExChannelInteractionMatrix.h"

DEFINE_LOG_CATEGORY_STATIC(LogPCGExChannelMatrix, Log, All);

FPCGExChannelInteractionMatrix::FPCGExChannelInteractionMatrix()
{
	// Default: every cell is Block. Compile() overlays the authored profile
	// entries on top; cells the user didn't touch stay Block, matching the
	// Q3-locked "Block by default" policy.
	for (int32 i = 0; i < MaxChannels; ++i)
	{
		for (int32 j = 0; j < MaxChannels; ++j)
		{
			Responses[i][j] = EPCGExChannelResponse::Block;
		}
	}
}

void FPCGExChannelInteractionMatrix::Compile(
	TConstArrayView<FName> InChannelKeys,
	TConstArrayView<FPCGExChannelProfile> InProfiles)
{
	// Reset to all-Block (the per-pair default). Authoring overrides only
	// flip cells the user explicitly listed.
	for (int32 i = 0; i < MaxChannels; ++i)
	{
		for (int32 j = 0; j < MaxChannels; ++j)
		{
			Responses[i][j] = EPCGExChannelResponse::Block;
		}
	}

	// Snapshot channel keys (capped at MaxChannels -- excess registered
	// channels can't be addressed by the uint32 mask anyway; if we ever
	// need more, widen the mask).
	ChannelKeys.Reset();
	const int32 Count = FMath::Min(InChannelKeys.Num(), MaxChannels);
	ChannelKeys.Reserve(Count);
	for (int32 i = 0; i < Count; ++i) { ChannelKeys.Add(InChannelKeys[i]); }

	if (InChannelKeys.Num() > MaxChannels)
	{
		UE_LOG(LogPCGExChannelMatrix, Warning,
			TEXT("Channel registry exceeds MaxChannels (%d > %d); excess channels are unaddressable in the runtime mask. Widen the mask type or trim the registry."),
			InChannelKeys.Num(), MaxChannels);
	}

	// Apply each profile's response overrides. Drop profiles or entries
	// referring to unknown channels with a warning; the safe direction is
	// "leave the cell at Block".
	for (const FPCGExChannelProfile& Profile : InProfiles)
	{
		const int32 CandidateBit = GetChannelBit(Profile.ChannelKey);
		if (CandidateBit == INDEX_NONE)
		{
			UE_LOG(LogPCGExChannelMatrix, Warning,
				TEXT("Channel profile references unknown channel '%s'; dropping."),
				*Profile.ChannelKey.ToString());
			continue;
		}

		for (const FPCGExChannelResponseEntry& Entry : Profile.Responses)
		{
			const int32 StoredBit = GetChannelBit(Entry.StoredChannel);
			if (StoredBit == INDEX_NONE)
			{
				UE_LOG(LogPCGExChannelMatrix, Warning,
					TEXT("Channel profile '%s' references unknown stored channel '%s'; dropping that response entry."),
					*Profile.ChannelKey.ToString(), *Entry.StoredChannel.ToString());
				continue;
			}
			Responses[CandidateBit][StoredBit] = Entry.Response;
		}
	}
}

int32 FPCGExChannelInteractionMatrix::GetChannelBit(FName ChannelKey) const
{
	if (ChannelKey.IsNone()) { return INDEX_NONE; }

	for (int32 i = 0; i < ChannelKeys.Num(); ++i)
	{
		if (ChannelKeys[i].IsEqual(ChannelKey, ENameCase::IgnoreCase)) { return i; }
	}
	return INDEX_NONE;
}

FName FPCGExChannelInteractionMatrix::GetChannelKey(int32 BitIndex) const
{
	return ChannelKeys.IsValidIndex(BitIndex) ? ChannelKeys[BitIndex] : NAME_None;
}

bool FPCGExChannelInteractionMatrix::ShouldRunNarrowPhase(uint32 CandidateMask, uint32 StoredMask) const
{
	// Safe default: no channel info on either side means we run narrow phase
	// (preserves pre-channel-matrix behavior for un-channeled entries / tests).
	if (CandidateMask == 0 || StoredMask == 0) { return true; }

	// Walk the cross-product of set bits. First Block wins -- as soon as any
	// (candidate, stored) channel pair is Block, the narrow phase has to run.
	// Only the all-Ignore case lets us skip.
	uint32 CMask = CandidateMask;
	while (CMask != 0)
	{
		const uint32 CBit = FMath::CountTrailingZeros(CMask);
		CMask &= CMask - 1;  // clear lowest set bit

		uint32 SMask = StoredMask;
		while (SMask != 0)
		{
			const uint32 SBit = FMath::CountTrailingZeros(SMask);
			SMask &= SMask - 1;

			if (Responses[CBit][SBit] == EPCGExChannelResponse::Block) { return true; }
		}
	}

	return false;
}
