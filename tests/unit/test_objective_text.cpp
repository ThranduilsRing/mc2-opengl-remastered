// tests/unit/test_objective_text.cpp
//
// The objective text reader / writer (code/objective_text.cpp) and the boolean flag table (code/objective_flags.cpp), step 4 batch B1.1
// (.claude/cleanup/cxx23-step4-b1-notes.md, hazards H1, H2, H15). Both are leaf TUs over FitIniFile, so the real file stack runs here.
//
// The rules pinned below:
//  - a missing or over-long key is an empty optional, so a missing ObjectiveModel never takes the text of the key read before it;
//  - a writer always writes its key, an empty value included;
//  - the line-break conversion keeps the character after the last break;
//  - a flag reference is its ID, else its decimal index;
//  - flag IDs match ignoring ASCII case.

#include "doctest.h"

#include "inifile.h"
#include "objective_flags.h"
#include "objective_text.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <string>

namespace
{

// A FIT file in memory; the buffer must outlive the file (the reader aliases it).
struct MemFit
{
	std::string text;
	FitIniFile fit;
	explicit MemFit(std::string body) : text("FITini\r\n\r\n" + std::move(body) + "\r\nFITend\r\n")
	{
		REQUIRE(fit.open(text.data(), (int)text.size()) == NO_ERR);
	}
	~MemFit() { fit.close(); }
};

std::string scratchPath(const char* name)
{
	return std::string("mvh_scratch_objtext_") + name;
}

std::string slurp(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_SUITE("objective text")
{

TEST_CASE("a missing key is an empty optional, an empty value is an empty string")
{
	MemFit f("[Obj]\r\nst Present = \"here\"\r\nst Empty = \"\"\r\n");
	REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
	REQUIRE(objective_text::read(f.fit, "Present").has_value());
	CHECK(*objective_text::read(f.fit, "Present") == "here");
	CHECK_FALSE(objective_text::read(f.fit, "Absent").has_value());
	REQUIRE(objective_text::read(f.fit, "Empty").has_value());
	CHECK(objective_text::read(f.fit, "Empty")->empty());
}

TEST_CASE("line breaks: every one converts and the text after the last one is kept")
{
	using namespace objective_text;
	CHECK(toMemoryBreaks("a\nb") == "a\r\nb");                  // the character after the last break survives
	CHECK(toMemoryBreaks("a\nb\nc") == "a\r\nb\r\nc");
	CHECK(toMemoryBreaks("a\n") == "a\r\n");
	CHECK(toMemoryBreaks("") == "");
	CHECK(toMemoryBreaks("no breaks") == "no breaks");
	CHECK(toFileBreaks("a\r\nb") == "a\nb");
	CHECK(toFileBreaks("a\r\nb\r\n") == "a\nb\n");
	CHECK(toFileBreaks("a\r\nbc") == "a\nbc");
	CHECK(toFileBreaks("lone \r stays") == "lone \r stays");
}

TEST_CASE("a value longer than the reader's buffer is rejected, not cut")
{
	std::string longValue(objective_text::kMaxTextLength + 50, 'x');
	MemFit f("[Obj]\r\nst Long = \"" + longValue + "\"\r\nst Fits = \"ok\"\r\n");
	REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
	CHECK_FALSE(objective_text::read(f.fit, "Long").has_value());
	std::optional<std::string> fits = objective_text::read(f.fit, "Fits");
	REQUIRE(fits.has_value());
	CHECK(*fits == "ok");
}

TEST_CASE("the length limit is inclusive: exactly kMaxTextLength characters are read, one more is rejected")
{
	const std::string atLimit(objective_text::kMaxTextLength, 'y');
	const std::string overLimit(objective_text::kMaxTextLength + 1, 'z');
	MemFit f("[Obj]\r\nst AtLimit = \"" + atLimit + "\"\r\nst OverLimit = \"" + overLimit + "\"\r\nul FlagIndex = 3\r\n");
	REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
	std::optional<std::string> value = objective_text::read(f.fit, "AtLimit");
	REQUIRE(value.has_value());
	CHECK(value->size() == objective_text::kMaxTextLength);
	std::string checked = "stale";
	CHECK(objective_text::readChecked(f.fit, "OverLimit", checked) == objective_text::ReadStatus::TooLong);
	CHECK(checked.empty());
	CHECK(objective_text::readChecked(f.fit, "Absent", checked) == objective_text::ReadStatus::Absent);
	CHECK(objective_text::readChecked(f.fit, "AtLimit", checked) == objective_text::ReadStatus::Read);
	CHECK(checked.size() == objective_text::kMaxTextLength);
	// An over-long flag ID is read as absent, so the numeric index is the fallback.
	CHECK(objective_text::readFlagID(f.fit, "OverLimit", "FlagIndex") == std::optional<std::string>("3"));
}

TEST_CASE("readPlain: the value as stored, absent and over-long are empty optionals")
{
	MemFit f("[Obj]\r\nst Video = \"intro\"\r\nst Empty = \"\"\r\nst Long = \"0123456789\"\r\n");
	REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
	CHECK(objective_text::readPlain(f.fit, "Video") == std::optional<std::string>("intro"));
	CHECK(objective_text::readPlain(f.fit, "Empty") == std::optional<std::string>(""));
	CHECK_FALSE(objective_text::readPlain(f.fit, "Absent").has_value());
	CHECK_FALSE(objective_text::readPlain(f.fit, "Long", 9).has_value());                     // 10 characters, limit 9
	CHECK(objective_text::readPlain(f.fit, "Long", 10) == std::optional<std::string>("0123456789"));
}

TEST_CASE("a flag reference is its ID, else its decimal index")
{
	SUBCASE("the ID wins over an index")
	{
		MemFit f("[Obj]\r\nst FlagID = \"gate\"\r\nul FlagIndex = 4\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		CHECK(objective_text::readFlagID(f.fit, "FlagID", "FlagIndex") == std::optional<std::string>("gate"));
	}
	SUBCASE("only an index")
	{
		MemFit f("[Obj]\r\nul FlagIndex = 4\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		CHECK(objective_text::readFlagID(f.fit, "FlagID", "FlagIndex") == std::optional<std::string>("4"));
	}
	SUBCASE("neither")
	{
		MemFit f("[Obj]\r\nst Title = \"T\"\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		CHECK_FALSE(objective_text::readFlagID(f.fit, "FlagID", "FlagIndex").has_value());
	}
}

TEST_CASE("the writer always writes the key, with line breaks stored as a bare line feed")
{
	const std::string path = scratchPath("roundtrip.fit");
	{
		FitIniFile out;
		REQUIRE(out.create(path.c_str()) == NO_ERR);
		out.writeBlock("Obj");
		objective_text::write(out, "Empty", "");
		objective_text::write(out, "Text", "two\r\nlines");
		out.close();
	}
	const std::string bytes = slurp(path);
	CHECK(bytes.find("st Empty = \"\"") != std::string::npos);                  // an empty value is still a key
	CHECK(bytes.find("st Text = \"two\nlines\"") != std::string::npos);       // stored with a bare line feed

	FitIniFile in;
	REQUIRE(in.open(path.c_str()) == NO_ERR);
	REQUIRE(in.seekBlock("Obj") == NO_ERR);
	REQUIRE(objective_text::read(in, "Empty").has_value());
	CHECK(objective_text::read(in, "Empty")->empty());
	// FitIniFile's own reader joins the first two lines of a quoted value without a break, so a multi-line text is not asserted back here.
	in.close();
	std::remove(path.c_str());
}

TEST_CASE("objective fields: the rule for each missing key")
{
	SUBCASE("every key present")
	{
		MemFit f("[Obj]\r\nst Title = \"T\"\r\nst Description = \"D\"\r\nst ActivateFlagID = \"flagA\"\r\n"
		         "st ResetStatusFlagID = \"flagR\"\r\nst ObjectiveModel = \"model1\"\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		objective_text::Fields fields;
		objective_text::readFields(f.fit, fields);
		CHECK(fields.title == "T");
		CHECK(fields.description == "D");
		CHECK(fields.activateFlagID == "flagA");
		CHECK(fields.resetStatusFlagID == "flagR");
		CHECK(fields.modelName == "model1");
	}
	SUBCASE("no Description: it is the Title; no ObjectiveModel: empty, not the key read before it")
	{
		MemFit f("[Obj]\r\nst Title = \"Only a title\"\r\nst ActivateFlagID = \"flagA\"\r\nst ResetStatusFlagID = \"flagR\"\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		objective_text::Fields fields;
		objective_text::readFields(f.fit, fields);
		CHECK(fields.title == "Only a title");
		CHECK(fields.description == "Only a title");
		CHECK(fields.modelName.empty());                         // not "flagR", the key read just before
	}
	SUBCASE("an empty Description is a Description, not an absent one")
	{
		MemFit f("[Obj]\r\nst Title = \"T\"\r\nst Description = \"\"\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		objective_text::Fields fields;
		objective_text::readFields(f.fit, fields);
		CHECK(fields.title == "T");
		CHECK(fields.description.empty());
	}
	SUBCASE("a file from before flags had names: ActivateFlagIndex becomes the ID")
	{
		MemFit f("[Obj]\r\nst Title = \"T\"\r\nul ActivateFlagIndex = 7\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		objective_text::Fields fields;
		objective_text::readFields(f.fit, fields);
		CHECK(fields.activateFlagID == "7");
	}
	SUBCASE("no flag at all and no Title: the fields keep their values, the rest is empty")
	{
		MemFit f("[Obj]\r\nl Other = 1\r\n");
		REQUIRE(f.fit.seekBlock("Obj") == NO_ERR);
		objective_text::Fields fields;
		fields.activateFlagID = "kept";
		fields.resetStatusFlagID = "alsoKept";
		objective_text::readFields(f.fit, fields);
		CHECK(fields.activateFlagID == "kept");
		CHECK(fields.resetStatusFlagID == "alsoKept");
		CHECK(fields.title.empty());
		CHECK(fields.description.empty());
		CHECK(fields.modelName.empty());
	}
}

TEST_CASE("boolean flags: IDs match ignoring ASCII case, an unknown flag reads false")
{
	CBooleanArray flags;
	CHECK_FALSE(flags.getElementValue("never set"));
	flags.setElementValue("BridgeDown", true);
	CHECK(flags.getElementValue("bridgedown"));                 // mission data spells the same flag in different case
	CHECK(flags.getElementValue("BRIDGEDOWN"));
	flags.setElementValue("bridgeDOWN", false);                 // the same flag, not a second one
	CHECK_FALSE(flags.getElementValue("BridgeDown"));
	flags.setElementValue("other", true);
	CHECK(flags.getElementValue("Other"));
	CHECK_FALSE(flags.getElementValue("bridge"));               // a prefix is not the flag
	flags.Clear();
	CHECK_FALSE(flags.getElementValue("other"));
}

TEST_CASE("boolean flags load: one over-long ID is skipped, the flags after it still load")
{
	const std::string tooLong(objective_text::kMaxTextLength + 1, 'x');
	MemFit f("[Team1BooleanFlags]\r\nst BooleanString0 = \"first\"\r\nb BooleanValue0 = 1\r\n"
	         "st BooleanString1 = \"" + tooLong + "\"\r\nb BooleanValue1 = 1\r\n"
	         "st BooleanString2 = \"third\"\r\nb BooleanValue2 = 1\r\n");
	CBooleanArray flags;
	flags.load(1, &f.fit);
	CHECK(flags.getElementValue("first"));
	CHECK(flags.getElementValue("third"));
}

TEST_CASE("boolean flags save in insertion order and load back")
{
	const std::string path = scratchPath("flags.fit");
	{
		CBooleanArray flags;
		flags.setElementValue("zeta", true);
		flags.setElementValue("Alpha", false);
		flags.setElementValue("mid", true);
		FitIniFile out;
		REQUIRE(out.create(path.c_str()) == NO_ERR);
		flags.save(2, &out);
		out.close();
	}
	const std::string bytes = slurp(path);
	CHECK(bytes.find("[Team2BooleanFlags]") != std::string::npos);
	CHECK(bytes.find("st BooleanString0 = \"zeta\"") < bytes.find("st BooleanString1 = \"Alpha\""));
	CHECK(bytes.find("st BooleanString1 = \"Alpha\"") < bytes.find("st BooleanString2 = \"mid\""));

	CBooleanArray loaded;
	FitIniFile in;
	REQUIRE(in.open(path.c_str()) == NO_ERR);
	loaded.load(2, &in);
	CHECK(loaded.getElementValue("ZETA"));
	CHECK_FALSE(loaded.getElementValue("alpha"));
	CHECK(loaded.getElementValue("Mid"));
	CBooleanArray otherTeam;
	otherTeam.load(1, &in);                                      // no such block: nothing added, no failure
	CHECK_FALSE(otherTeam.getElementValue("zeta"));
	in.close();
	std::remove(path.c_str());
}

TEST_CASE("boolean flags: a flag without its value loads as false")
{
	MemFit f("[Team0BooleanFlags]\r\nst BooleanString0 = \"lonely\"\r\n");
	CBooleanArray flags;
	flags.load(0, &f.fit);
	CHECK_FALSE(flags.getElementValue("lonely"));
}

}
