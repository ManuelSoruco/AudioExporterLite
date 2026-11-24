// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Text/AudioFactoryCsvUtils.h"
namespace AudioExporter {

FString AudioFactoryCsvUtils::EscapeCsvField(const FString& InValue)
{
	FString Escaped = InValue;

	// Neutralize spreadsheet formula injection. Excel/Sheets interpret a cell whose first
	// character is =, +, -, @ (or a leading tab/CR) as a formula, regardless of CSV quoting.
	// Prefix such values with an apostrophe so the content is rendered literally instead of
	// executed.
	if (!Escaped.IsEmpty())
	{
		const TCHAR FirstChar = Escaped[0];
		if (FirstChar == TEXT('=') || FirstChar == TEXT('+') || FirstChar == TEXT('-')
			|| FirstChar == TEXT('@') || FirstChar == TEXT('\t') || FirstChar == TEXT('\r'))
		{
			Escaped.InsertAt(0, TEXT('\''));
		}
	}

	Escaped.ReplaceInline(TEXT("\""), TEXT("\"\""));
	return FString::Printf(TEXT("\"%s\""), *Escaped);
}
} // namespace AudioExporter
