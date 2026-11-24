// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#pragma once

#include "CoreMinimal.h"

#ifndef AF_UTILS_API
#define AF_UTILS_API
#endif

/**
 * AudioFactoryCsvUtils
 *
 * Product-neutral CSV field escaping, shared by every product's report writer. Promoted
 * from four independent private copies (Converter, Pro, Normalizer, Lite) into this one
 * canonical implementation so there is exactly one place that decides how a report cell is
 * made spreadsheet-safe.
 */
namespace AudioExporter {
namespace AudioFactoryCsvUtils
{
	/**
	 * Escapes one CSV field for spreadsheet safety and always quotes it.
	 *
	 * Formula-injection neutralization: a cell whose first character is = + - @ (or a leading
	 * tab/CR) is prefixed with an apostrophe so Excel/Sheets render it literally instead of
	 * executing it as a formula, regardless of CSV quoting. Embedded double quotes are then
	 * doubled per RFC 4180 and the whole field is wrapped in quotes.
	 *
	 * @param InValue  Raw field text (may contain commas, quotes, newlines, or a formula-like prefix).
	 * @return The field, formula-neutralized, quote-doubled, and wrapped in a leading/trailing '"'.
	 */
	AF_UTILS_API FString EscapeCsvField(const FString& InValue);
}
} // namespace AudioExporter
