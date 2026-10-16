// tests/unit/test_estring.cpp
//
// EString::Format sizes its buffer with the C runtime's own format parser (vsnprintf with a null buffer). The hand-rolled
// %-scanner it replaced did not know ll / z / j and then vsprintf wrote into a buffer sized by that guess.
//
// The second half pins what the class does today so that converting a caller to std::string is judged against recorded
// behaviour. Those tests describe the OLD class and stay until the class is deleted with the gui widgets.

#include "doctest.h"

#include "estring.h"

#include <cstring>
#include <string>

#include <windows.h>

TEST_SUITE("EString")
{

TEST_CASE("Format handles the length modifiers the old scanner mis-sized")
{
	EString s;
	s.Format("%lld|%zu|%I64d|%5.2f|%s|%c", 1234567890123LL, (size_t)42, (__int64)-7, 3.14159, "hi", 'x');
	CHECK(std::strcmp(s.Data(), "1234567890123|42|-7| 3.14|hi|x") == 0);
	CHECK(s.Length() == (int)std::strlen("1234567890123|42|-7| 3.14|hi|x"));
}

TEST_CASE("Format grows past the initial buffer and reuses the string")
{
	EString s("short");
	char big[600];
	std::memset(big, 'z', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	s.Format("[%s]", big);
	CHECK(s.Length() == (int)sizeof(big) - 1 + 2);
	CHECK(s.Data()[0] == '[');
	CHECK(s.Data()[s.Length() - 1] == ']');

	s.Format("%d", 7);                              // shrinks back; no stale tail
	CHECK(std::strcmp(s.Data(), "7") == 0);
	CHECK(s.Length() == 1);
}

TEST_CASE("Format of an empty template and of a width-padded argument")
{
	EString s("x");
	s.Format("%s", "");
	CHECK(s.Length() == 0);
	s.Format("%-8s|%8s|", "ab", "cd");
	CHECK(std::strcmp(s.Data(), "ab      |      cd|") == 0);
}

// Data() is null for a string that was never assigned or was emptied; ANY assignment of a literal, even "", allocates.
TEST_CASE("H1: Data() is null only for a never-assigned or emptied string")
{
	EString never;
	CHECK(never.Data() == nullptr);
	CHECK(static_cast<const char*>(never) == nullptr);
	CHECK(never.IsEmpty());
	CHECK(never.Length() == 0);

	EString assignedEmpty;
	assignedEmpty = "";
	REQUIRE(assignedEmpty.Data() != nullptr);
	CHECK(assignedEmpty.Data()[0] == '\0');
	CHECK(assignedEmpty.IsEmpty());

	EString ctorEmpty("");
	CHECK(ctorEmpty.Data() != nullptr);

	EString formatted;
	formatted.Format("%s", "");
	CHECK(formatted.Data() != nullptr);

	EString emptied("text");
	REQUIRE(emptied.Data() != nullptr);
	emptied.Empty();
	CHECK(emptied.Data() == nullptr);

	EString assignedNull("text");
	const char* np = nullptr;
	assignedNull = np;
	CHECK(assignedNull.Data() == nullptr);
	CHECK(assignedNull.IsEmpty());
}

// A null C string is a legal, empty EString. std::string(nullptr) is undefined, so converted code goes through mc2::str::from_c.
TEST_CASE("H13: EString of a null pointer is empty, not undefined")
{
	const char* np = nullptr;
	EString viaCtor(np);
	CHECK(viaCtor.Data() == nullptr);
	CHECK(viaCtor.Length() == 0);
	CHECK(viaCtor == "");
}

// Equality, the case-flagged Compare and operator< all end in the Windows lstrcmpi, so they ignore case. Even Compare(x, true)
// ("case sensitive") only short-circuits on identical leading bytes and otherwise falls through to the case-insensitive compare.
TEST_CASE("H2/H3: equality and order ignore case")
{
	CHECK(EString("Abc") == "abc");
	CHECK(EString("Abc") == EString("aBC"));
	CHECK("abc" == EString("ABC"));
	CHECK_FALSE(EString("Abc") != "abc");
	CHECK(EString("Abc").Compare("abc") == 0);
	CHECK(EString("Abc").Compare("abc", false) == 0);
	CHECK(EString("Abc").Compare("abc", true) == 0);
	CHECK(EString("abc").Compare("abd") < 0);
	CHECK(EString("abd").Compare("ABC") > 0);

	CHECK_FALSE(EString("Zeta") < EString("alpha"));
	CHECK(EString("alpha") < EString("Zeta"));
	CHECK(EString("Zeta") > EString("alpha"));

	// A prefix is smaller; the empty string is smaller than anything and equal only to empty (also a never-assigned one).
	CHECK(EString("ab").Compare("abc") < 0);
	CHECK(EString("abc").Compare("ab") > 0);
	CHECK(EString("").Compare("a") < 0);
	CHECK(EString().Compare("") == 0);
	CHECK(EString("a").Compare("") > 0);
	CHECK_FALSE(EString("abc") == "abcd");

	// The free operator<= with a C string on the left is a strict less-than (an equal pair is not "<=").
	CHECK_FALSE("abc" <= EString("abc"));
	CHECK("abc" <= EString("abd"));
}

// MakeUpper / MakeLower go through the ANSI code page (CharUpper / CharLower), so a byte above 0x7F changes case under cp1252.
// std::string has no such behaviour: mc2::str::ascii_upper leaves those bytes alone (tests/unit/test_mc2str.cpp).
TEST_CASE("H12: MakeUpper and MakeLower follow the ANSI code page")
{
	EString s("aBc-1_x");
	s.MakeUpper();
	CHECK(std::strcmp(s.Data(), "ABC-1_X") == 0);
	s.MakeLower();
	CHECK(std::strcmp(s.Data(), "abc-1_x") == 0);

	if (GetACP() == 1252)
	{
		EString accented("\xE9");
		accented.MakeUpper();
		CHECK(static_cast<unsigned char>(accented.Data()[0]) == 0xC9);
	}
}

// Index conventions. SubString and Remove take an INCLUSIVE end index; Left / Right take counts; Find returns -1; Replace overwrites.
TEST_CASE("H6: index conventions of SubString, Remove, Left, Right, Find, Replace, Insert")
{
	EString s("hello");
	CHECK(std::strcmp(s.SubString(1, 3).Data(), "ell") == 0);
	CHECK(std::strcmp(s.SubString(2, 2).Data(), "l") == 0);
	CHECK(std::strcmp(s.Left(2).Data(), "he") == 0);
	CHECK(std::strcmp(s.Right(2).Data(), "lo") == 0);

	CHECK(s.Find('l') == 2);
	CHECK(s.Find('l', 3) == 3);
	CHECK(s.Find('z') == EString::INVALID_INDEX);
	CHECK(s.Find("lo") == 3);
	CHECK(s.Find("zz") == -1);
	CHECK(s.ReverseFind('l') == 3);
	CHECK(s.ReverseFind('z') == -1);

	EString removed("hello");
	CHECK(removed.Remove(1, 2));
	CHECK(std::strcmp(removed.Data(), "hlo") == 0);
	CHECK(removed.Length() == 3);
	CHECK_FALSE(removed.Remove(2, 5));          // the end index must lie inside the string
	CHECK(std::strcmp(removed.Data(), "hlo") == 0);

	EString gone("hello world");
	CHECK(gone.Remove(EString("lo w")));
	CHECK(std::strcmp(gone.Data(), "helorld") == 0);

	EString overwritten("hello");
	overwritten.Replace(1, "EY");
	CHECK(std::strcmp(overwritten.Data(), "hEYlo") == 0);

	EString inserted("hello");
	inserted.Insert(5, "!");
	CHECK(std::strcmp(inserted.Data(), "hello!") == 0);
	inserted.Insert(0, ">");
	CHECK(std::strcmp(inserted.Data(), ">hello!") == 0);
	inserted += " ok";
	CHECK(std::strcmp(inserted.Data(), ">hello! ok") == 0);
}

// Length() stops at an embedded NUL (lstrlen); Size() is the stored count.
TEST_CASE("H7: Length stops at an embedded NUL, Size does not")
{
	EString s("abc");
	s[1] = '\0';
	CHECK(s.Length() == 1);
	CHECK(s.Size() == 3);
}

// Compare(x, true) reads strlen(x) bytes from the receiver before it looks at the receiver's length, so an argument longer than the
// receiver's allocation over-reads. Only an in-bounds case is run here; std::string has no such read.
TEST_CASE("H8: a shorter receiver compares smaller than a longer argument")
{
	CHECK(EString("ab").Compare("abc", true) < 0);
}

// Copies share one buffer, so a pointer taken from one copy outlives the original. A std::string pointer does not
// (the clang-cl lifetime gate, tests/analyze/analyze_dangling.py, and the accessor rule cover that).
TEST_CASE("H14: a pointer into a shared buffer outlives the source string")
{
	auto* source = new EString("shared text");
	EString copy(*source);
	const char* p = source->Data();
	delete source;
	CHECK(std::strcmp(p, "shared text") == 0);          // alive through `copy`; ASan reports a use-after-free if the buffer were not shared
	CHECK(p == copy.Data());
}

// A by-value accessor returns a copy that shares the member's buffer, so the caller's `const char*` stays valid as long as the owner lives.
TEST_CASE("H4: a pointer from a by-value accessor stays valid while the owner lives")
{
	struct Holder
	{
		EString name{"member"};
		EString Name() const { return name; }
	} holder;
	const char* p = holder.Name();                       // the temporary dies here; its buffer is the member's
	CHECK(std::strcmp(p, "member") == 0);
	CHECK(p == holder.name.Data());
}

}
