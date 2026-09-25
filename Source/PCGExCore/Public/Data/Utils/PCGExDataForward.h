// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExDataFilterDetails.h"
#include "PCGExDataForwardDetails.h"
#include "Types/PCGExAttributeIdentity.h"

namespace PCGExMT
{
	struct FScope;
}

namespace PCGExData
{
	struct FConstPoint;
	class IBuffer;
	class FDataForwardHandler;
	class IAttributeBroadcaster;
}

namespace PCGExData
{
	class PCGEXCORE_API FDataForwardHandler
	{
		FPCGExForwardDetails Details;
		TSharedPtr<FFacade> SourceDataFacade;
		TSharedPtr<FFacade> TargetDataFacade;
		TArray<FAttributeIdentity> Identities;
		TArray<TSharedPtr<IBuffer>> Readers;
		TArray<TSharedPtr<IBuffer>> Writers;
		EForwardDomain Domain = EForwardDomain::ToData;

		/** Identifier the source attribute is written under on the target, per Domain. */
		FPCGAttributeIdentifier GetTargetIdentifier(const FAttributeIdentity& Identity) const;

		/** True when Domain moves this attribute to a different domain than its source one. */
		bool RedirectsDomain(const FAttributeIdentity& Identity) const;

	public:
		using FValidateFn = std::function<bool(const FAttributeIdentity&)>;

		~FDataForwardHandler() = default;
		FDataForwardHandler(const FPCGExForwardDetails& InDetails, const TSharedPtr<FFacade>& InSourceDataFacade, const EForwardDomain InDomain = EForwardDomain::ToData);
		// InIgnoredAttributes: source attribute names never forwarded, applied before the details name filter.
		FDataForwardHandler(const FPCGExForwardDetails& InDetails, const TSharedPtr<FFacade>& InSourceDataFacade, const TSharedPtr<FFacade>& InTargetDataFacade, const EForwardDomain InDomain = EForwardDomain::ToData, const TSet<FName>* InIgnoredAttributes = nullptr);

		void ValidateIdentities(FValidateFn&& Fn);

		bool IsEmpty() const
		{
			return Identities.IsEmpty();
		}

		const TArray<FAttributeIdentity>& GetIdentities() const
		{
			return Identities;
		}

		void Forward(const int32 SourceIndex, const int32 TargetIndex);

		// Prepared-target variant (requires the target-facade constructor): fans one source row out to many
		// target indices through the pre-created writers -- no lazy buffer creation, safe from concurrent tasks.
		void Forward(const int32 SourceIndex, const TArray<int32>& Indices);

		// Prepared-target variant over a contiguous target range.
		void Forward(const int32 SourceIndex, const PCGExMT::FScope& TargetScope);

		void Forward(const int32 SourceIndex, const TSharedPtr<FFacade>& InTargetDataFacade);

		// Fans one source row out to the given target indices. With EForwardDomain::ToElements a @Data source lands
		// per element, so successive calls with disjoint index sets carry distinct values on one target data.
		void Forward(const int32 SourceIndex, const TSharedPtr<FFacade>& InTargetDataFacade, const TArray<int32>& Indices);
		void Forward(const int32 SourceIndex, UPCGMetadata* InTargetMetadata);

		// Forwards source attributes onto a single entry (per-row, element domain) of any target metadata -- e.g. a source path's @Data onto one attribute-set row.
		void Forward(const int32 SourceIndex, UPCGMetadata* InTargetMetadata, const int64 TargetKey);
	};
}
