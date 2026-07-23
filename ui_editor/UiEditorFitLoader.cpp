/***************************************************************
 * FILENAME: UiEditorFitLoader.cpp
 * DESCRIPTION: Minimal read-only FIT layout loader for the MC2R UI Editor.
 *
 * AUTHOR: Methuselas
 * CREATED: 2026-05-19
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-19
 *
 * CHANGES:
 * - Added typed-block FIT parsing for GuiPage and Gui* cell rectangles.
 * - Improved failed-load status output with the attempted FIT path.
 * - Prefer legacyLocalRect for editor artboard display when present.
 * - Added page mount offset parsing for aligned composite previews.
 * - Added uvPixels parsing so previews sample legacy atlas slices.
 * - Added source-preserving Save Copy output for edited rect fields.
 * - Added legacy selected/checked state parsing for truth-viewer previews.
 * - Added editable text font/color/anchor parsing and GuiRedirect support.
 * - Persisted generated text pulse/flash authoring fields.
 * - Loaded and saved text effect/source diagnostics for legacy flashing text.
 * - Loaded legacy button state UVs, text placement rects, and per-state text colors.
 * - Suppressed legacy undefined-string placeholders from normal preview fields.
 ***************************************************************/

#include "UiEditorFitLoader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace
{
	struct ParsedBlock
	{
		std::string type;
		std::map<std::string, std::string> values;
	};

	std::string ToLowerCopy(const std::string& value);

	std::string Trim(const std::string& value)
	{
		std::string::size_type begin = 0;
		while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
			++begin;

		std::string::size_type end = value.size();
		while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
			--end;

		return value.substr(begin, end - begin);
	}

	bool StartsWith(const std::string& value, const char* prefix)
	{
		const std::string prefixString(prefix);
		return value.size() >= prefixString.size()
			&& value.compare(0, prefixString.size(), prefixString) == 0;
	}

	std::string StripQuotes(const std::string& value)
	{
		std::string result = Trim(value);
		const bool quoted = result.size() >= 2 && result.front() == '"' && result.back() == '"';
		if (quoted)
			result = result.substr(1, result.size() - 2);

		std::string unescaped;
		unescaped.reserve(result.size());

		for (std::size_t i = 0; i < result.size(); ++i)
		{
			if (result[i] == '\\' && i + 1 < result.size())
			{
				const char next = result[i + 1];
				if (next == 'n')
				{
					unescaped.push_back('\n');
					++i;
					continue;
				}
				if (next == 't')
				{
					unescaped.push_back('\t');
					++i;
					continue;
				}
				if (next == '"' || next == '\\')
				{
					unescaped.push_back(next);
					++i;
					continue;
				}
			}

			unescaped.push_back(result[i]);
		}

		return unescaped;
	}

	bool ParseAssignment(const std::string& line, std::string& key, std::string& value)
	{
		const std::string::size_type equals = line.find('=');
		if (equals == std::string::npos)
			return false;

		key = Trim(line.substr(0, equals));
		value = StripQuotes(line.substr(equals + 1));

		return !key.empty();
	}


	bool ParseFloatScalar(const std::string& value, float& out)
	{
		std::istringstream stream(StripQuotes(value));
		stream >> out;
		return !stream.fail();
	}

	bool ParseFloatList4(const std::string& value, float& a, float& b, float& c, float& d)
	{
		std::string normalized = value;
		std::replace(normalized.begin(), normalized.end(), ',', ' ');

		std::istringstream stream(normalized);
		return static_cast<bool>(stream >> a >> b >> c >> d);
	}

	bool ParseFloatList2(const std::string& value, float& a, float& b)
	{
		std::string normalized = value;
		std::replace(normalized.begin(), normalized.end(), ',', ' ');

		std::istringstream stream(normalized);
		return static_cast<bool>(stream >> a >> b);
	}

	int ParseIntValue(const std::string& value, int fallback)
	{
		char* end = nullptr;
		const long parsed = std::strtol(value.c_str(), &end, 10);
		if (end == value.c_str())
			return fallback;
		return static_cast<int>(parsed);
	}

	bool ParseBoolValue(const std::string& value, bool fallback)
	{
		if (value.empty())
			return fallback;

		const std::string lower = ToLowerCopy(StripQuotes(value));
		if (lower == "true" || lower == "yes" || lower == "1")
			return true;
		if (lower == "false" || lower == "no" || lower == "0")
			return false;

		return fallback;
	}

	bool ParseColorArgbValue(const std::string& value, float outColor[4])
	{
		if (value.empty())
			return false;

		std::string text = StripQuotes(Trim(value));
		if (text.empty())
			return false;

		unsigned long parsed = 0;
		if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		{
			char* end = nullptr;
			parsed = std::strtoul(text.c_str(), &end, 16);
			if (end == text.c_str())
				return false;
		}
		else
		{
			char* end = nullptr;
			const long signedValue = std::strtol(text.c_str(), &end, 10);
			if (end == text.c_str())
				return false;
			parsed = static_cast<unsigned long>(static_cast<unsigned int>(signedValue));
		}

		const unsigned int argb = static_cast<unsigned int>(parsed);
		const unsigned int a = (argb >> 24) & 0xff;
		const unsigned int r = (argb >> 16) & 0xff;
		const unsigned int g = (argb >> 8) & 0xff;
		const unsigned int b = argb & 0xff;

		outColor[0] = static_cast<float>(r) / 255.0f;
		outColor[1] = static_cast<float>(g) / 255.0f;
		outColor[2] = static_cast<float>(b) / 255.0f;
		outColor[3] = static_cast<float>(a) / 255.0f;
		return true;
	}

std::string GetValue(const ParsedBlock& block, const char* key);

	void CopyColor(float dst[4], const float src[4])
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		dst[3] = src[3];
	}

	bool IsUndefinedLegacyStringKey(const std::string& value)
	{
		const std::string lower = ToLowerCopy(value);
		return lower.find("ids_undefined_string") != std::string::npos
			|| lower.find("legacy.strings.ids_undefined") != std::string::npos;
	}

	bool ParseTextRectFields(const ParsedBlock& block, UiEditorFitCell& cell)
	{
		const std::string explicitRect = GetValue(block, "textRect");
		if (!explicitRect.empty())
		{
			if (ParseFloatList4(explicitRect, cell.textX, cell.textY, cell.textWidth, cell.textHeight)
				&& cell.textWidth > 0.0f
				&& cell.textHeight > 0.0f)
			{
				return true;
			}
		}

		const std::string x = GetValue(block, "textX");
		const std::string y = GetValue(block, "textY");
		const std::string w = GetValue(block, "textWidth");
		const std::string h = GetValue(block, "textHeight");
		if (!x.empty() && !y.empty() && !w.empty() && !h.empty())
		{
			return ParseFloatScalar(x, cell.textX)
				&& ParseFloatScalar(y, cell.textY)
				&& ParseFloatScalar(w, cell.textWidth)
				&& ParseFloatScalar(h, cell.textHeight)
				&& cell.textWidth > 0.0f
				&& cell.textHeight > 0.0f;
		}

		return false;
	}


	bool ParseNamedRectField(const ParsedBlock& block, const char* fieldName, float& x, float& y, float& w, float& h)
	{
		const std::string explicitRect = GetValue(block, fieldName);
		return !explicitRect.empty()
			&& ParseFloatList4(explicitRect, x, y, w, h)
			&& w > 0.0f
			&& h > 0.0f;
	}


	std::string FormatBoolValue(bool value)
	{
		return value ? "true" : "false";
	}

	std::string FormatColorArgbValue(const float color[4])
	{
		const unsigned int a = static_cast<unsigned int>(std::max(0.0f, std::min(1.0f, color[3])) * 255.0f + 0.5f);
		const unsigned int r = static_cast<unsigned int>(std::max(0.0f, std::min(1.0f, color[0])) * 255.0f + 0.5f);
		const unsigned int g = static_cast<unsigned int>(std::max(0.0f, std::min(1.0f, color[1])) * 255.0f + 0.5f);
		const unsigned int b = static_cast<unsigned int>(std::max(0.0f, std::min(1.0f, color[2])) * 255.0f + 0.5f);

		std::ostringstream stream;
		stream << "0x" << std::hex << std::nouppercase
			<< std::setw(2) << std::setfill('0') << a
			<< std::setw(2) << std::setfill('0') << r
			<< std::setw(2) << std::setfill('0') << g
			<< std::setw(2) << std::setfill('0') << b;
		return stream.str();
	}

	std::string DirectoryNameFromPath(const std::string& path)
	{
		const std::string::size_type slash = path.find_last_of("/\\");
		if (slash == std::string::npos)
			return std::string();
		return path.substr(0, slash + 1);
	}

	bool IsAbsoluteOrDataPath(const std::string& path)
	{
		if (path.empty())
			return false;
		if (path.size() > 1 && path[1] == ':')
			return true;
		if (path[0] == '/' || path[0] == '\\')
			return true;
		const std::string lower = ToLowerCopy(path);
		return StartsWith(lower, "data/") || StartsWith(lower, "data\\") || StartsWith(lower, "../data/") || StartsWith(lower, "..\\data\\");
	}

	bool TryReadRedirectTarget(const char* path, std::string& outTarget)
	{
		outTarget.clear();

		std::ifstream file(path);
		if (!file.is_open())
			return false;

		bool inRedirect = false;
		std::string line;
		while (std::getline(file, line))
		{
			const std::string trimmed = Trim(line);
			if (trimmed.empty() || StartsWith(trimmed, "//"))
				continue;

			if (!inRedirect)
			{
				const std::string::size_type brace = trimmed.find('{');
				if (brace == std::string::npos)
					continue;

				const std::string blockType = Trim(trimmed.substr(0, brace));
				inRedirect = blockType == "GuiRedirect";
				continue;
			}

			if (trimmed == "}")
				return !outTarget.empty();

			std::string key;
			std::string value;
			if (ParseAssignment(trimmed, key, value)
				&& (key == "target" || key == "targetFile" || key == "targetPath"))
			{
				outTarget = value;
			}
		}

		return !outTarget.empty();
	}

	std::string GetValue(const ParsedBlock& block, const char* key)
	{
		const std::map<std::string, std::string>::const_iterator it = block.values.find(key);
		if (it == block.values.end())
			return std::string();
		return it->second;
	}


	void ExpandLayoutExtents(UiEditorFitLayout& layout, const UiEditorFitCell& cell)
	{
		const int right = static_cast<int>(cell.x + cell.width + 0.5f);
		const int bottom = static_cast<int>(cell.y + cell.height + 0.5f);

		if (right > layout.designWidth)
			layout.designWidth = right;

		if (bottom > layout.designHeight)
			layout.designHeight = bottom;
	}

	void InferResolutionFromPath(const std::string& path, UiEditorFitLayout& layout)
	{
		std::string lower = path;
		for (std::size_t i = 0; i < lower.size(); ++i)
			lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));

		struct Candidate
		{
			const char* token;
			int width;
			int height;
		};

		static const Candidate candidates[] =
		{
			{ "1920", 1920, 1080 },
			{ "1600", 1600, 1200 },
			{ "1280", 1280, 1024 },
			{ "1024", 1024, 768 },
			{ "800", 800, 600 },
			{ "640", 640, 480 },
		};

		for (std::size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
		{
			if (lower.find(candidates[i].token) != std::string::npos)
			{
				layout.designWidth = std::max(layout.designWidth, candidates[i].width);
				layout.designHeight = std::max(layout.designHeight, candidates[i].height);
				return;
			}
		}
	}


	std::string ToLowerCopy(const std::string& value)
	{
		std::string result = value;
		for (std::size_t i = 0; i < result.size(); ++i)
			result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
		return result;
	}

	std::string FormatFloat(float value)
	{
		std::ostringstream stream;
		stream << std::fixed << std::setprecision(3) << value;

		std::string result = stream.str();
		while (result.size() > 1 && result.find('.') != std::string::npos && result.back() == '0')
			result.erase(result.size() - 1);

		if (!result.empty() && result.back() == '.')
			result.erase(result.size() - 1);

		return result;
	}

	std::string FormatRect(float x, float y, float width, float height)
	{
		return FormatFloat(x) + ", " + FormatFloat(y) + ", " + FormatFloat(width) + ", " + FormatFloat(height);
	}

	std::string FormatFitStringValue(const std::string& value)
	{
		std::string result = "\"";
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const char c = value[i];
			if (c == '\\')
				result += "\\\\";
			else if (c == '"')
				result += "\\\"";
			else if (c == '\n')
				result += "\\n";
			else if (c == '\t')
				result += "\\t";
			else
				result.push_back(c);
		}
		result += "\"";
		return result;
	}

	std::string LeadingWhitespace(const std::string& value)
	{
		std::string::size_type count = 0;
		while (count < value.size() && std::isspace(static_cast<unsigned char>(value[count])))
			++count;
		return value.substr(0, count);
	}

	bool SamePathText(const std::string& left, const std::string& right)
	{
		return ToLowerCopy(left) == ToLowerCopy(right);
	}

	std::string GuessLayerForBlock(const ParsedBlock& block)
	{
		if (StartsWith(block.type, "GuiButton") || block.type == "UiV2Button" ||
		    block.type == "UiV2ComboBox" || block.type == "UiV2DropList" ||
		    block.type == "UiV2Edit")
			return "controls";

		if (StartsWith(block.type, "GuiText") || StartsWith(block.type, "GuiList") ||
		    block.type == "UiV2Text")
			return "content";

		if (StartsWith(block.type, "GuiImage") || StartsWith(block.type, "GuiStatic") ||
		    block.type == "UiV2Static" || block.type == "UiV2ButtonChild" ||
		    block.type == "UiV2AnimObject")
			return "art";
		if (StartsWith(block.type, "GuiRect") || block.type == "UiV2Rect")
			return "layout";

		return "content";
	}

	std::string BuildFallbackKey(const ParsedBlock& block, std::size_t index)
	{
		std::ostringstream stream;
		stream << block.type << "." << index;
		return stream.str();
	}

	void ConsumeBlock(const ParsedBlock& block, UiEditorFitLayout& layout)
	{
		if (block.type == "GuiPage" || block.type == "UiV2Page")
		{
			const std::string key = GetValue(block, "key");
			const std::string title = GetValue(block, "title");

			if (!key.empty())
				layout.key = key;

			if (!title.empty())
				layout.title = title;

			layout.designWidth = ParseIntValue(GetValue(block, "localWidth"), layout.designWidth);
			layout.designHeight = ParseIntValue(GetValue(block, "localHeight"), layout.designHeight);
			ParseFloatList2(GetValue(block, "mountOffset"), layout.mountOffsetX, layout.mountOffsetY);

			const std::string overlayValue = GetValue(block, "overlayGameOS");
			if (!overlayValue.empty())
				layout.overlaysGameOS = ParseBoolValue(overlayValue, layout.overlaysGameOS);

			const std::string renderMode = GetValue(block, "renderMode");
			if (!renderMode.empty())
				layout.renderMode = renderMode;

			const std::string backgroundPolicy = GetValue(block, "backgroundPolicy");
			if (!backgroundPolicy.empty())
				layout.backgroundPolicy = backgroundPolicy;

			const std::string baseLayoutPath = GetValue(block, "baseLayout");
			if (!baseLayoutPath.empty())
				layout.baseLayoutPath = baseLayoutPath;

			const std::string compositionMode = GetValue(block, "compositionMode");
			if (!compositionMode.empty())
				layout.compositionMode = compositionMode;

			return;
		}

		if (!StartsWith(block.type, "Gui") && !StartsWith(block.type, "UiV2"))
			return;

		// V2 metadata blocks are not renderable cells.
		if (block.type == "UiV2FamilyCount" || block.type == "UiV2Cursor" ||
		    block.type == "UiV2SourceBlock" || block.type == "UiV2Animation")
			return;

		const std::string rect = GetValue(block, "rect");
		const std::string localRect = GetValue(block, "legacyLocalRect");
		const std::string displayRect = !localRect.empty() ? localRect : rect;
		if (displayRect.empty())
			return;

		UiEditorFitCell cell;
		cell.type = block.type;
		cell.key = GetValue(block, "key");
		cell.pageKey = GetValue(block, "pageKey");
		cell.role = GetValue(block, "role");
		cell.layer = GetValue(block, "layer");
		cell.texture = GetValue(block, "texture");
		cell.textKey = GetValue(block, "textKey");
		cell.visibleText = GetValue(block, "visibleText");
		cell.textAlign = GetValue(block, "textAlign");
		cell.textAnchor = GetValue(block, "textAnchor");
		cell.font = GetValue(block, "font");
		cell.fontStyle = GetValue(block, "fontStyle");
		ParseFloatScalar(GetValue(block, "fontSize"), cell.fontSize);
		cell.textEffect = GetValue(block, "textEffect");
		cell.legacyTextEffectSource = GetValue(block, "legacyTextEffectSource");
		cell.bold = ParseBoolValue(GetValue(block, "bold"), cell.bold);
		cell.italic = ParseBoolValue(GetValue(block, "italic"), cell.italic);
		if (!ParseColorArgbValue(GetValue(block, "colorArgb"), cell.textColor))
			ParseColorArgbValue(GetValue(block, "legacyColor"), cell.textColor);
		cell.hasNormalTextColor = ParseColorArgbValue(GetValue(block, "normalTextColorArgb"), cell.normalTextColor);
		cell.hasPressedTextColor = ParseColorArgbValue(GetValue(block, "pressedTextColorArgb"), cell.pressedTextColor);
		cell.hasHighlightTextColor = ParseColorArgbValue(GetValue(block, "highlightTextColorArgb"), cell.highlightTextColor);
		cell.hasDisabledTextColor = ParseColorArgbValue(GetValue(block, "disabledTextColorArgb"), cell.disabledTextColor);
		cell.buttonOverlayEnabled = ParseBoolValue(GetValue(block, "buttonOverlayEnabled"), cell.buttonOverlayEnabled);
		ParseFloatScalar(GetValue(block, "buttonOverlayAlpha"), cell.buttonOverlayAlpha);
		cell.hasNormalOverlayColor = ParseColorArgbValue(GetValue(block, "normalOverlayColorArgb"), cell.normalOverlayColor);
		cell.hasPressedOverlayColor = ParseColorArgbValue(GetValue(block, "pressedOverlayColorArgb"), cell.pressedOverlayColor);
		cell.hasHighlightOverlayColor = ParseColorArgbValue(GetValue(block, "highlightOverlayColorArgb"), cell.highlightOverlayColor);
		cell.hasDisabledOverlayColor = ParseColorArgbValue(GetValue(block, "disabledOverlayColorArgb"), cell.disabledOverlayColor);
		cell.hasTextRect = ParseTextRectFields(block, cell);
		cell.hasArtRect = ParseNamedRectField(block, "artRect", cell.artX, cell.artY, cell.artWidth, cell.artHeight);
		cell.hasBorderRect = ParseNamedRectField(block, "borderRect", cell.borderX, cell.borderY, cell.borderWidthRect, cell.borderHeightRect);
		cell.borderEnabled = ParseBoolValue(GetValue(block, "borderEnabled"), cell.borderEnabled);
		cell.buttonTemplate = GetValue(block, "buttonTemplate");
		cell.linkedImageKey = GetValue(block, "linkedImageKey");
		cell.linkedButtonKey = GetValue(block, "linkedButtonKey");
		cell.buttonTemplatePart = GetValue(block, "buttonTemplatePart");
		cell.runtimeTextBinding = GetValue(block, "runtimeTextBinding");
		cell.runtimeTextSource = GetValue(block, "runtimeTextSource");
		cell.legacyTemplateTextKey = GetValue(block, "legacyTemplateTextKey");
		cell.legacyTemplateVisibleText = GetValue(block, "legacyTemplateVisibleText");
		cell.widgetType = GetValue(block, "widgetType");
		cell.sourceControlType = GetValue(block, "sourceControlType");
		cell.sourceStyle = GetValue(block, "sourceStyle");
		cell.sourceLine = GetValue(block, "sourceLine");
		cell.legacySection = GetValue(block, "legacySection");
		cell.legacyComment = GetValue(block, "legacyComment");
		cell.legacyContext = GetValue(block, "legacyContext");
		cell.legacyRectSource = GetValue(block, "legacyRectSource");
		cell.helpCaptionKey = GetValue(block, "helpCaptionKey");
		cell.helpCaptionText = GetValue(block, "helpCaptionText");
		cell.helpDescKey = GetValue(block, "helpDescKey");
		cell.helpDescText = GetValue(block, "helpDescText");
		if (IsUndefinedLegacyStringKey(cell.textKey))
		{
			cell.textKey.clear();
			cell.visibleText.clear();
		}
		if (IsUndefinedLegacyStringKey(cell.helpCaptionKey))
		{
			cell.helpCaptionKey.clear();
			cell.helpCaptionText.clear();
		}
		if (IsUndefinedLegacyStringKey(cell.helpDescKey))
		{
			cell.helpDescKey.clear();
			cell.helpDescText.clear();
		}
		cell.controlId = GetValue(block, "controlId");
		cell.actionType = GetValue(block, "actionType");
		cell.targetPageKey = GetValue(block, "targetPageKey");
		cell.targetMissionKey = GetValue(block, "targetMissionKey");
		cell.transitionName = GetValue(block, "transitionName");
		cell.legacyMessage = GetValue(block, "legacyMessage");
		cell.aliasOverride = GetValue(block, "aliasOverride");
		cell.textMissing = ToLowerCopy(GetValue(block, "textMissing")) == "true";
		if (cell.textKey.empty() && cell.visibleText.empty())
			cell.textMissing = false;
		cell.visible = ToLowerCopy(GetValue(block, "visible")) != "false";
		cell.wrapText = ParseBoolValue(GetValue(block, "wrapText"), cell.wrapText);
		cell.toggleButton = ParseBoolValue(GetValue(block, "toggleButton"), cell.toggleButton);
		cell.checked = ParseBoolValue(GetValue(block, "checked"), cell.checked);
		cell.selectedState = ParseBoolValue(GetValue(block, "selectedState"), cell.selectedState);
		cell.disabledState = ParseBoolValue(GetValue(block, "disabledState"), cell.disabledState);
		cell.pulseEnabled = ParseBoolValue(GetValue(block, "pulseEnabled"), cell.pulseEnabled);
		ParseFloatScalar(GetValue(block, "pulseSpeed"), cell.pulseSpeed);
		ParseFloatScalar(GetValue(block, "borderWidth"), cell.borderWidth);
		if (GetValue(block, "borderEnabled").empty())
			cell.borderEnabled = cell.borderWidth > 0.0f;
		ParseFloatScalar(GetValue(block, "opacity"), cell.opacity);
		cell.rectRaw = displayRect;
		cell.rectFieldName = !localRect.empty() ? "legacyLocalRect" : "rect";
		if (cell.layer.empty())
			cell.layer = GuessLayerForBlock(block);

		if (cell.key.empty())
			cell.key = BuildFallbackKey(block, layout.cells.size());

		if (!ParseFloatList4(displayRect, cell.x, cell.y, cell.width, cell.height))
			return;

		if (!cell.hasArtRect)
		{
			cell.hasArtRect = true;
			cell.artX = cell.x;
			cell.artY = cell.y;
			cell.artWidth = cell.width;
			cell.artHeight = cell.height;
		}

		if (!cell.hasBorderRect)
		{
			cell.hasBorderRect = true;
			cell.borderX = cell.x;
			cell.borderY = cell.y;
			cell.borderWidthRect = cell.width;
			cell.borderHeightRect = cell.height;
		}

		if (cell.buttonTemplate.empty() &&
		    (ToLowerCopy(cell.type).find("guibutton") != std::string::npos || cell.type == "UiV2Button"))
			cell.buttonTemplate = "composite_hit_art_border_text_v1";

		if (GetValue(block, "wrapText").empty()
			&& (StartsWith(cell.type, "GuiText") || StartsWith(cell.type, "GuiList") || StartsWith(cell.type, "GuiEditBox")
			    || cell.type == "UiV2Text" || cell.type == "UiV2Edit")
			&& cell.height > std::max(24.0f, cell.fontSize * 1.6f))
		{
			cell.wrapText = true;
		}

		if (cell.type == "GuiList")
		{
			cell.isListCell = true;
			ParseFloatScalar(GetValue(block, "itemHeight"), cell.itemHeight);
			ParseFloatScalar(GetValue(block, "itemOffsetX"), cell.itemOffsetX);
			ParseFloatScalar(GetValue(block, "itemOffsetY"), cell.itemOffsetY);
			if (!GetValue(block, "highlightColorArgb").empty())
				ParseColorArgbValue(GetValue(block, "highlightColorArgb"), cell.highlightColor);
			const std::string previewCount = GetValue(block, "listPreviewItemCount");
			if (!previewCount.empty())
				cell.listPreviewItemCount = ParseIntValue(previewCount, 3);
		}

		// Legacy conversion emits non-visual container/animation bookkeeping blocks
		// with 0x0 rectangles. Keeping those as selectable cells makes the editor
		// show phantom HUD entries at 0,0 and distorts layout diagnostics. They are
		// metadata, not drawable/editable widgets.
		if (cell.width <= 0.0f || cell.height <= 0.0f)
			return;

		const std::string uvPixels = GetValue(block, "uvPixels");
		if (!uvPixels.empty())
		{
			cell.hasUvPixels = ParseFloatList4(uvPixels, cell.uvX, cell.uvY, cell.uvWidth, cell.uvHeight);
		}
		else
		{
			// Most converted legacy GameOS FITs store atlas slices as separate
			// uvX/uvY/uvWidth/uvHeight fields instead of a packed uvPixels tuple.
			// Treat either form as the same legacy atlas source rectangle so
			// editor preview matches the in-game sliced-image behavior.
			const std::string uvX = GetValue(block, "uvX");
			const std::string uvY = GetValue(block, "uvY");
			const std::string uvWidth = GetValue(block, "uvWidth");
			const std::string uvHeight = GetValue(block, "uvHeight");
			if (!uvX.empty() && !uvY.empty() && !uvWidth.empty() && !uvHeight.empty())
			{
				cell.hasUvPixels =
					ParseFloatScalar(uvX, cell.uvX)
					&& ParseFloatScalar(uvY, cell.uvY)
					&& ParseFloatScalar(uvWidth, cell.uvWidth)
					&& ParseFloatScalar(uvHeight, cell.uvHeight)
					&& cell.uvWidth > 0.0f
					&& cell.uvHeight > 0.0f;
			}
		}

		cell.hasPressedUvPixels =
			cell.hasUvPixels
			&& ParseFloatScalar(GetValue(block, "uvPressedX"), cell.uvPressedX)
			&& ParseFloatScalar(GetValue(block, "uvPressedY"), cell.uvPressedY);
		cell.hasHighlightUvPixels =
			cell.hasUvPixels
			&& ParseFloatScalar(GetValue(block, "uvHighlightX"), cell.uvHighlightX)
			&& ParseFloatScalar(GetValue(block, "uvHighlightY"), cell.uvHighlightY);
		cell.hasDisabledUvPixels =
			cell.hasUvPixels
			&& ParseFloatScalar(GetValue(block, "uvDisabledX"), cell.uvDisabledX)
			&& ParseFloatScalar(GetValue(block, "uvDisabledY"), cell.uvDisabledY);

		ExpandLayoutExtents(layout, cell);
		layout.cells.push_back(cell);
	}
}

UiEditorFitCell::UiEditorFitCell()
	: fontSize(14.0f)
	, hasNormalTextColor(false)
	, hasPressedTextColor(false)
	, hasHighlightTextColor(false)
	, hasDisabledTextColor(false)
	, buttonOverlayEnabled(false)
	, buttonOverlayAlpha(0.35f)
	, hasNormalOverlayColor(false)
	, hasPressedOverlayColor(false)
	, hasHighlightOverlayColor(false)
	, hasDisabledOverlayColor(false)
	, hasTextRect(false)
	, textX(0.0f)
	, textY(0.0f)
	, textWidth(0.0f)
	, textHeight(0.0f)
	, bold(false)
	, italic(false)
	, textMissing(false)
	, visible(true)
	, locked(false)
	, wrapText(false)
	, toggleButton(false)
	, checked(false)
	, selectedState(false)
	, disabledState(false)
	, pulseEnabled(false)
	, pulseSpeed(1.5f)
	, borderWidth(0.0f)
	, borderEnabled(false)
	, hasBorderRect(false)
	, borderX(0.0f)
	, borderY(0.0f)
	, borderWidthRect(0.0f)
	, borderHeightRect(0.0f)
	, hasArtRect(false)
	, artX(0.0f)
	, artY(0.0f)
	, artWidth(0.0f)
	, artHeight(0.0f)
	, opacity(1.0f)
	, rectFieldName("rect")
	, x(0.0f)
	, y(0.0f)
	, width(0.0f)
	, height(0.0f)
	, hasUvPixels(false)
	, uvX(0.0f)
	, uvY(0.0f)
	, uvWidth(0.0f)
	, uvHeight(0.0f)
	, hasPressedUvPixels(false)
	, uvPressedX(0.0f)
	, uvPressedY(0.0f)
	, hasHighlightUvPixels(false)
	, uvHighlightX(0.0f)
	, uvHighlightY(0.0f)
	, hasDisabledUvPixels(false)
	, uvDisabledX(0.0f)
	, uvDisabledY(0.0f)
	, isListCell(false)
	, itemHeight(0.0f)
	, itemOffsetX(2.0f)
	, itemOffsetY(0.0f)
	, listPreviewItemCount(3)
{
	textAlign = "left";
	textAnchor = "top_left";
	font = "Liberation Sans Regular";
	fontStyle = "Regular";
	textColor[0] = 1.0f;
	textColor[1] = 1.0f;
	textColor[2] = 1.0f;
	textColor[3] = 1.0f;
	CopyColor(normalTextColor, textColor);
	CopyColor(pressedTextColor, textColor);
	CopyColor(highlightTextColor, textColor);
	CopyColor(disabledTextColor, textColor);
	CopyColor(normalOverlayColor, textColor);
	CopyColor(pressedOverlayColor, textColor);
	CopyColor(highlightOverlayColor, textColor);
	CopyColor(disabledOverlayColor, textColor);
	fillColor[0] = 0.0f;
	fillColor[1] = 0.0f;
	fillColor[2] = 0.0f;
	fillColor[3] = 0.0f;
	borderColor[0] = 0.0f;
	borderColor[1] = 0.0f;
	borderColor[2] = 0.0f;
	borderColor[3] = 0.0f;
	highlightColor[0] = 0.95f;
	highlightColor[1] = 0.55f;
	highlightColor[2] = 0.12f;
	highlightColor[3] = 0.50f;
}

UiEditorFitLayout::UiEditorFitLayout()
	: loaded(false)
	, designWidth(800)
	, designHeight(600)
	, mountOffsetX(0.0f)
	, mountOffsetY(0.0f)
	, overlaysGameOS(true)
{
	renderMode = "gameos_overlay";
	backgroundPolicy = "transparent_overlay";
	compositionMode = "overlay";
}

bool UiEditorFitLoadLayout(const char* path, UiEditorFitLayout& outLayout)
{
	UiEditorFitLayout nextLayout;

	if (path == nullptr || path[0] == '\0')
	{
		outLayout = nextLayout;
		outLayout.statusMessage = "No FIT path supplied.";
		return false;
	}

	std::string redirectTarget;
	if (TryReadRedirectTarget(path, redirectTarget))
	{
		std::string resolvedTarget = redirectTarget;
		if (!IsAbsoluteOrDataPath(resolvedTarget))
			resolvedTarget = DirectoryNameFromPath(path) + resolvedTarget;

		UiEditorFitLayout redirected;
		if (UiEditorFitLoadLayout(resolvedTarget.c_str(), redirected))
		{
			redirected.statusMessage = std::string("Loaded redirect: ") + path + " -> " + resolvedTarget;
			outLayout = redirected;
			return true;
		}

		outLayout = nextLayout;
		outLayout.sourcePath = path;
		outLayout.statusMessage = std::string("Redirect target failed: ") + resolvedTarget;
		return false;
	}

	std::ifstream file(path);
	if (!file.is_open())
	{
		outLayout = nextLayout;
		outLayout.sourcePath = path;
		outLayout.statusMessage = std::string("Could not open FIT layout: ") + path;
		return false;
	}

	nextLayout.sourcePath = path;
	InferResolutionFromPath(nextLayout.sourcePath, nextLayout);

	bool inBlock = false;
	ParsedBlock block;

	std::string line;
	while (std::getline(file, line))
	{
		std::string trimmed = Trim(line);
		if (trimmed.empty() || StartsWith(trimmed, "//"))
			continue;

		if (!inBlock)
		{
			const std::string::size_type brace = trimmed.find('{');
			if (brace == std::string::npos)
				continue;

			block = ParsedBlock();
			block.type = Trim(trimmed.substr(0, brace));
			inBlock = !block.type.empty();
			continue;
		}

		if (trimmed == "}")
		{
			ConsumeBlock(block, nextLayout);
			inBlock = false;
			continue;
		}

		std::string key;
		std::string value;
		if (ParseAssignment(trimmed, key, value))
			block.values[key] = value;
	}

	// Re-apply filename/profile inference after parsing. Some converted legacy
	// pages still declare screen_800x600 even when their filename and coordinates
	// are 1024/1280/1600-era layouts; the file/profile token must win for editor
	// canvas sizing so those pages preview in their intended coordinate space.
	InferResolutionFromPath(nextLayout.sourcePath, nextLayout);

	nextLayout.loaded = true;
	nextLayout.statusMessage = "Loaded FIT layout.";

	if (nextLayout.key.empty())
		nextLayout.key = "unknown.layout";

	if (nextLayout.title.empty())
		nextLayout.title = nextLayout.key;

	outLayout = nextLayout;
	return true;
}


bool UiEditorFitSaveLayoutCopy(const UiEditorFitLayout& layout, const char* path, std::string& outStatus)
{
	outStatus.clear();

	if (!layout.loaded)
	{
		outStatus = "No loaded FIT layout to save.";
		return false;
	}

	if (layout.sourcePath.empty())
	{
		outStatus = "Loaded layout has no source path.";
		return false;
	}

	if (path == nullptr || path[0] == '\0')
	{
		outStatus = "No Save Copy path supplied.";
		return false;
	}

	const std::string outputPath(path);
	if (SamePathText(layout.sourcePath, outputPath))
	{
		outStatus = "Save Copy refuses to overwrite the loaded source file.";
		return false;
	}

	std::ifstream input(layout.sourcePath.c_str());
	if (!input.is_open())
	{
		outStatus = std::string("Could not reopen source FIT: ") + layout.sourcePath;
		return false;
	}

	std::vector<std::string> lines;
	std::string line;
	while (std::getline(input, line))
		lines.push_back(line);

	std::map<std::string, std::map<std::string, std::string> > fieldUpdates;
	for (std::size_t i = 0; i < layout.cells.size(); ++i)
	{
		const UiEditorFitCell& cell = layout.cells[i];
		if (cell.key.empty())
			continue;

		std::map<std::string, std::string>& updates = fieldUpdates[cell.key];
		const std::string fieldName = cell.rectFieldName.empty() ? std::string("rect") : cell.rectFieldName;
		updates[fieldName] = FormatRect(cell.x, cell.y, cell.width, cell.height);
		updates["visibleText"] = FormatFitStringValue(cell.visibleText);
		updates["textKey"] = FormatFitStringValue(cell.textKey);
		updates["textAlign"] = FormatFitStringValue(cell.textAlign);
		updates["textAnchor"] = FormatFitStringValue(cell.textAnchor);
		updates["wrapText"] = FormatBoolValue(cell.wrapText);
		updates["font"] = FormatFitStringValue(cell.font);
		updates["fontStyle"] = FormatFitStringValue(cell.fontStyle);
		updates["fontSize"] = FormatFloat(cell.fontSize);
		updates["textEffect"] = FormatFitStringValue(cell.textEffect);
		updates["legacyTextEffectSource"] = FormatFitStringValue(cell.legacyTextEffectSource);
		updates["bold"] = FormatBoolValue(cell.bold);
		updates["italic"] = FormatBoolValue(cell.italic);
		updates["colorArgb"] = FormatColorArgbValue(cell.textColor);
		updates["normalTextColorArgb"] = FormatColorArgbValue(cell.normalTextColor);
		updates["pressedTextColorArgb"] = FormatColorArgbValue(cell.pressedTextColor);
		updates["highlightTextColorArgb"] = FormatColorArgbValue(cell.highlightTextColor);
		updates["disabledTextColorArgb"] = FormatColorArgbValue(cell.disabledTextColor);
		updates["buttonOverlayEnabled"] = FormatBoolValue(cell.buttonOverlayEnabled);
		updates["buttonOverlayAlpha"] = FormatFloat(cell.buttonOverlayAlpha);
		updates["borderEnabled"] = FormatBoolValue(cell.borderEnabled);
		updates["borderRect"] = FormatFitStringValue(FormatRect(cell.borderX, cell.borderY, cell.borderWidthRect, cell.borderHeightRect));
		updates["artRect"] = FormatFitStringValue(FormatRect(cell.artX, cell.artY, cell.artWidth, cell.artHeight));
		updates["buttonTemplate"] = FormatFitStringValue(cell.buttonTemplate);
		updates["linkedImageKey"] = FormatFitStringValue(cell.linkedImageKey);
		updates["linkedButtonKey"] = FormatFitStringValue(cell.linkedButtonKey);
		updates["buttonTemplatePart"] = FormatFitStringValue(cell.buttonTemplatePart);
		updates["normalOverlayColorArgb"] = FormatColorArgbValue(cell.normalOverlayColor);
		updates["pressedOverlayColorArgb"] = FormatColorArgbValue(cell.pressedOverlayColor);
		updates["highlightOverlayColorArgb"] = FormatColorArgbValue(cell.highlightOverlayColor);
		updates["disabledOverlayColorArgb"] = FormatColorArgbValue(cell.disabledOverlayColor);
		updates["textRect"] = FormatFitStringValue(FormatRect(cell.textX, cell.textY, cell.textWidth, cell.textHeight));
		updates["textX"] = FormatFloat(cell.textX);
		updates["textY"] = FormatFloat(cell.textY);
		updates["textWidth"] = FormatFloat(cell.textWidth);
		updates["textHeight"] = FormatFloat(cell.textHeight);
		updates["pulseEnabled"] = FormatBoolValue(cell.pulseEnabled);
		updates["pulseSpeed"] = FormatFloat(cell.pulseSpeed);
		updates["texture"] = FormatFitStringValue(cell.texture);
		if (cell.type == "GuiList" || cell.isListCell)
		{
			updates["itemHeight"] = FormatFloat(cell.itemHeight);
			updates["itemOffsetX"] = FormatFloat(cell.itemOffsetX);
			updates["itemOffsetY"] = FormatFloat(cell.itemOffsetY);
			updates["highlightColorArgb"] = FormatColorArgbValue(cell.highlightColor);
			if (cell.listPreviewItemCount != 3)
			{
				char countBuf[16];
				std::snprintf(countBuf, sizeof(countBuf), "%d", cell.listPreviewItemCount);
				updates["listPreviewItemCount"] = countBuf;
			}
		}
	}

	bool inBlock = false;
	std::string blockKey;
	std::map<std::string, std::size_t> fieldLines;
	int updatedCount = 0;

	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		const std::string trimmed = Trim(lines[i]);

		if (!inBlock)
		{
			if (trimmed.find('{') != std::string::npos)
			{
				inBlock = true;
				blockKey.clear();
				fieldLines.clear();
			}

			continue;
		}

		if (trimmed == "}")
		{
			const std::map<std::string, std::map<std::string, std::string> >::const_iterator update = fieldUpdates.find(blockKey);
			if (update != fieldUpdates.end())
			{
				const std::string indent = LeadingWhitespace(lines[i]).empty() ? std::string("    ") : LeadingWhitespace(lines[i]) + "    ";
				for (std::map<std::string, std::string>::const_iterator valueIt = update->second.begin(); valueIt != update->second.end(); ++valueIt)
				{
					const std::map<std::string, std::size_t>::const_iterator lineIt = fieldLines.find(valueIt->first);
					if (lineIt != fieldLines.end())
					{
						lines[lineIt->second] = LeadingWhitespace(lines[lineIt->second]) + valueIt->first + " = " + valueIt->second;
						++updatedCount;
					}
					else if (valueIt->first != "legacyLocalRect" || update->second.find("legacyLocalRect") != update->second.end())
					{
						lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i), indent + valueIt->first + " = " + valueIt->second);
						++i;
						++updatedCount;
					}
				}
			}

			inBlock = false;
			continue;
		}

		std::string key;
		std::string value;
		if (!ParseAssignment(trimmed, key, value))
			continue;

		if (key == "key")
			blockKey = value;

		fieldLines[key] = i;
	}

	std::ofstream output(outputPath.c_str(), std::ios::trunc);
	if (!output.is_open())
	{
		outStatus = std::string("Could not create Save Copy output: ") + outputPath;
		return false;
	}

	for (std::size_t i = 0; i < lines.size(); ++i)
		output << lines[i] << '\n';

	std::ostringstream status;
	status << "Saved FIT copy: " << outputPath << " (" << updatedCount << " fields updated).";
	outStatus = status.str();
	return true;
}


const UiEditorFitCell* UiEditorFitGetCell(const UiEditorFitLayout& layout, int index)
{
	if (index < 0 || index >= static_cast<int>(layout.cells.size()))
		return nullptr;

	return &layout.cells[static_cast<std::size_t>(index)];
}
