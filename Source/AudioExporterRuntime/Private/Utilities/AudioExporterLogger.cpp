// Copyright 2026 AudioFactory. Licensed under the MIT License; see LICENSE.

#include "Utilities/AudioExporterLogger.h"
#include "Utilities/AudioExporterUtils.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogAudioExporter);

namespace
{
	/** One exact rule width shared by the banner, section rules, and footer. */
	constexpr const TCHAR* RuleLine = TEXT("================================================================");

	/** Status vocabulary used in reports and results; render labels only -- the row's real
	 * classification is EAudioExportRowStatus, never a compared string. */
	constexpr const TCHAR* StatusSucceeded = TEXT("Succeeded");
	constexpr const TCHAR* StatusSucceededGainLimited = TEXT("Succeeded, Gain Limited");
	constexpr const TCHAR* StatusFailed = TEXT("Failed");
	constexpr const TCHAR* StatusSkipped = TEXT("Skipped");

	void AppendLabelLine(FString& Text, const TCHAR* Label, const FString& Value)
	{
		// One label-column width constant for the whole file.
		Text += FString::Printf(TEXT("%-28s: %s\n"), Label, *Value);
	}

	FString NormalizePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return InPath;
		}
		return FPaths::ConvertRelativePathToFull(InPath);
	}

	/** Durations render as seconds below 60 s and as m:ss (h:mm:ss) above it. */
	FString FormatDuration(float Seconds)
	{
		if (Seconds >= 60.0f)
		{
			const int32 TotalSeconds = FMath::FloorToInt(Seconds);
			const int32 Hours = TotalSeconds / 3600;
			const int32 Minutes = (TotalSeconds % 3600) / 60;
			const int32 Secs = TotalSeconds % 60;
			return (Hours > 0)
				? FString::Printf(TEXT("%d:%02d:%02d"), Hours, Minutes, Secs)
				: FString::Printf(TEXT("%d:%02d"), Minutes, Secs);
		}
		return FString::Printf(TEXT("%.2f seconds"), Seconds);
	}

	/** Gains always carry an explicit sign. */
	FString FormatGain(float GainAppliedDb)
	{
		return FString::Printf(TEXT("%+.2f dB"), GainAppliedDb);
	}

	/** Peaks above the -1 dBFS normalization ceiling state it in words instead of an ASCII marker. */
	FString FormatPeak(float PeakDb)
	{
		if (PeakDb > -1.0f)
		{
			return FString::Printf(TEXT("%.2f dBFS (exceeds target)"), PeakDb);
		}
		return FString::Printf(TEXT("%.2f dBFS"), PeakDb);
	}

	FString FormatRms(float RMSDb)
	{
		return FString::Printf(TEXT("%.2f dBFS"), RMSDb);
	}

	/** Renders the measurement rows of one succeeded variation entry. */
	void AppendVariationDetails(FString& Text, const FAudioExportLogEntry& LogEntry)
	{
		if (LogEntry.bStaticSource)
		{
			Text += FString::Printf(
				TEXT("  Variation %d of %d (static source)\n"),
				LogEntry.Variation,
				FMath::Max(LogEntry.TotalVariations, 1));
		}
		else
		{
			Text += FString::Printf(TEXT("  Variation %d\n"), LogEntry.Variation);
		}
		if (!LogEntry.ExportedFileName.IsEmpty())
		{
			AppendLabelLine(Text, TEXT("Exported File Name"), LogEntry.ExportedFileName);
		}
		if (!LogEntry.OutputFilePath.IsEmpty())
		{
			AppendLabelLine(Text, TEXT("Output File Path"), NormalizePath(LogEntry.OutputFilePath));
		}
		AppendLabelLine(Text, TEXT("Duration"), FormatDuration(LogEntry.DurationSeconds));
		AppendLabelLine(Text, TEXT("Peak"), FormatPeak(LogEntry.ExportPeak));
		AppendLabelLine(Text, TEXT("RMS"), FormatRms(LogEntry.ExportRMS));
		AppendLabelLine(Text, TEXT("Gain Applied"), FormatGain(LogEntry.GainApplied));
		if (LogEntry.bGainLimited)
		{
			AppendLabelLine(Text, TEXT("Normalization Status"), StatusSucceededGainLimited);
			AppendLabelLine(Text, TEXT("Gain Requested"), FormatGain(LogEntry.GainRequested));
		}
	}

	/** Renders one non-success variation as a single human sentence, with no measurements. */
	void AppendVariationFailure(FString& Text, const FAudioExportLogEntry& LogEntry)
	{
		if (LogEntry.bStaticSource)
		{
			Text += FString::Printf(
				TEXT("  Variation %d of %d (static source)\n"),
				LogEntry.Variation,
				FMath::Max(LogEntry.TotalVariations, 1));
		}
		else
		{
			Text += FString::Printf(TEXT("  Variation %d\n"), LogEntry.Variation);
		}
		const FString Detail = LogEntry.Details.IsEmpty()
			? (LogEntry.Status == EAudioExportRowStatus::Skipped
				? TEXT("Save skipped because the export session is no longer active.")
				: TEXT("Failed to save WAV output."))
			: LogEntry.Details;
		AppendLabelLine(Text, TEXT("Details"), Detail);
	}

	/** The section an entry belongs to. Anything that is not Succeeded/Skipped renders as Failed --
	 * including Pending, which must never reach a written report. */
	const TCHAR* SectionForStatus(EAudioExportRowStatus Status)
	{
		switch (Status)
		{
		case EAudioExportRowStatus::Succeeded:
		case EAudioExportRowStatus::SucceededGainLimited:
			return StatusSucceeded;
		case EAudioExportRowStatus::Skipped:
			return StatusSkipped;
		default:
			return StatusFailed;
		}
	}

	void AppendOutcomeSection(FString& Text, const TArray<FAudioExportAssetLog>& LogsToExport, const TCHAR* OutcomeSection)
	{
		// Empty outcome sections are omitted.
		bool bHasRows = false;
		for (const FAudioExportAssetLog& AssetLog : LogsToExport)
		{
			for (const FAudioExportLogEntry& LogEntry : AssetLog.Variations)
			{
				if (SectionForStatus(LogEntry.Status) == OutcomeSection)
				{
					bHasRows = true;
					break;
				}
			}
			if (bHasRows)
			{
				break;
			}
		}

		if (!bHasRows)
		{
			return;
		}

		Text += FString::Printf(TEXT("\n%s\n"), OutcomeSection);
		Text += FString::Printf(TEXT("%s\n"), RuleLine);

		for (const FAudioExportAssetLog& AssetLog : LogsToExport)
		{
			bool bAssetHasRows = false;
			for (const FAudioExportLogEntry& LogEntry : AssetLog.Variations)
			{
				if (SectionForStatus(LogEntry.Status) == OutcomeSection)
				{
					bAssetHasRows = true;
					break;
				}
			}
			if (!bAssetHasRows)
			{
				continue;
			}

			AppendLabelLine(Text, TEXT("Source Asset Path"), AssetLog.SourceAssetPath);
			if (!AssetLog.RelativePath.IsEmpty())
			{
				const FString AudioRelativePath = FPaths::Combine(AudioExporterAudioSubfolder, AssetLog.RelativePath);
				AppendLabelLine(Text, TEXT("Export Sub-Folder"), AudioRelativePath);
			}

			for (const FAudioExportLogEntry& LogEntry : AssetLog.Variations)
			{
				if (SectionForStatus(LogEntry.Status) != OutcomeSection)
				{
					continue;
				}

				if (OutcomeSection == StatusSucceeded)
				{
					AppendVariationDetails(Text, LogEntry);
				}
				else
				{
					AppendVariationFailure(Text, LogEntry);
				}
			}

			Text += TEXT("\n");
		}
	}

	FString BuildTextReport(const TArray<FAudioExportAssetLog>& LogsToExport, const FString& InDate, const FString& SettingsSummary, const FString& InSessionFolder)
	{
		FString Text;

		// 1. Banner header: product + task, date (session id does not apply to this product; omitted).
		Text += FString::Printf(TEXT("%s\n"), RuleLine);
		Text += TEXT("=================== Audio Exporter Report ===================\n");
		Text += FString::Printf(TEXT("%s\n\n"), RuleLine);
		AppendLabelLine(Text, TEXT("Date"), InDate);

		// 2. Summary: honest counts with the shared Requested/Succeeded/Failed/Skipped vocabulary.
		int32 RequestedCount = 0;
		int32 SucceededCount = 0;
		int32 FailedCount = 0;
		int32 SkippedCount = 0;
		for (const FAudioExportAssetLog& AssetLog : LogsToExport)
		{
			for (const FAudioExportLogEntry& LogEntry : AssetLog.Variations)
			{
				++RequestedCount;
				switch (LogEntry.Status)
				{
				case EAudioExportRowStatus::Succeeded:
				case EAudioExportRowStatus::SucceededGainLimited:
					++SucceededCount;
					break;
				case EAudioExportRowStatus::Skipped:
					++SkippedCount;
					break;
				default:
					// Pending or Failed counts as failed; Pending never appears in a terminal report.
					++FailedCount;
					break;
				}
			}
		}

		Text += TEXT("\nSummary\n");
		Text += FString::Printf(TEXT("%s\n"), RuleLine);
		AppendLabelLine(Text, TEXT("Requested"), FString::FromInt(RequestedCount));
		AppendLabelLine(Text, TEXT("Succeeded"), FString::FromInt(SucceededCount));
		AppendLabelLine(Text, TEXT("Failed"), FString::FromInt(FailedCount));
		AppendLabelLine(Text, TEXT("Skipped"), FString::FromInt(SkippedCount));

		// 3. Settings Used.
		Text += TEXT("\nSettings Used\n");
		Text += FString::Printf(TEXT("%s\n"), RuleLine);
		if (!SettingsSummary.IsEmpty())
		{
			Text += SettingsSummary;
		}
		if (!Text.EndsWith(TEXT("\n")))
		{
			Text += TEXT("\n");
		}

		// 4. Per-asset sections grouped by outcome.
		AppendOutcomeSection(Text, LogsToExport, StatusSucceeded);
		AppendOutcomeSection(Text, LogsToExport, StatusSkipped);
		AppendOutcomeSection(Text, LogsToExport, StatusFailed);

		// 5. Footer: output location -- the session FOLDER, not the report file's own path,
		// normalized to an absolute path.
		Text += TEXT("\nOutput Location\n");
		Text += FString::Printf(TEXT("%s\n"), RuleLine);
		Text += FString::Printf(TEXT("%s\n"), *NormalizePath(InSessionFolder));
		Text += FString::Printf(TEXT("%s\n"), RuleLine);

		return Text;
	}

}

bool AudioExporterLogger::WriteTextLogFile(const TArray<FAudioExportAssetLog>& LogsToExport,
                                            const FString& InExportPath,
                                            const FString& InDate,
                                            const FString& SettingsSummary,
                                            const FString& InSessionFolder,
                                            FString* OutError)
{
	if (OutError)
	{
		OutError->Empty();
	}

	const FString TextData = BuildTextReport(LogsToExport, InDate, SettingsSummary, InSessionFolder);

	if (!FFileHelper::SaveStringToFile(TextData, *InExportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		const FString Error = FString::Printf(TEXT("Failed to write text report to '%s'."), *InExportPath);
		UE_LOG(LogAudioExporter, Error, TEXT("%s: %s"), *FString(__FUNCTION__), *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	UE_LOG(LogAudioExporter, Log, TEXT("Text log file successfully written to: %s"), *InExportPath);
	return true;
}
