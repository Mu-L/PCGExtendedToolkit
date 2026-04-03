// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Regex.h"

namespace PCGExRegex
{
	/** Lightweight wrapper around FRegexPattern. Compile once in Init(), call Test() per point. */
	struct FPattern
	{
		FPattern() {}
		~FPattern() {}

		/** Compiles the given pattern string. Returns false if the pattern is invalid. */
		bool Compile(const FString& InPattern)
		{
			CompiledPattern = MakeShared<FRegexPattern>(InPattern);
			return CompiledPattern.IsValid();
		}

		/** Tests whether the subject string matches the compiled pattern. */
		bool Test(const FString& Subject) const
		{
			check(CompiledPattern.IsValid());
			::FRegexMatcher Matcher(*CompiledPattern, Subject);
			return Matcher.FindNext();
		}

	private:
		TSharedPtr<FRegexPattern> CompiledPattern;
	};
}
