// tests/unit/test_gui_bridge.cpp
//
// Compile-time test of the widget text bridge (step 4 batch B1, .claude/cleanup/cxx23-step4-b1-notes.md section 2). The bridge is a
// const char* + const std::string& overload pair next to the EString one, so a caller holding std::string, a C string, a literal or an
// EString all resolve to one overload, and the resource-id form setText(0L) is not stolen by a pointer overload. A std::string_view overload
// would make every setText("literal") ambiguous, which is why there is none.
// Nothing here is linked or run: the widgets need the engine, only the overload resolution is checked, in a template so that an ambiguous
// call is a false requirement and not a hard error.

#include "doctest.h"

#include "aedit.h"
#include "alistbox.h"
#include "asystem.h"

#include <concepts>
#include <string>

namespace
{
template <class W> concept TakesLiteral = requires(W& w) { w.setText("x"); };
template <class W> concept TakesStdString = requires(W& w, const std::string& s) { w.setText(s); };
template <class W> concept TakesEString = requires(W& w, const EString& e) { w.setText(e); };
template <class W> concept TakesResourceId = requires(W& w) { w.setText(0L); };
template <class W> concept TakesCString = requires(W& w, const char* p) { w.setText(p); };

template <class E> concept EditTakesLiteral = requires(E& e) { e.setEntry("x"); };
template <class E> concept EditTakesStdString = requires(E& e, const std::string& s) { e.setEntry(s, 1); };
template <class E> concept EditTakesEString = requires(E& e, const EString& s) { e.setEntry(s); };
template <class E> concept EditReturnsStdString = requires(const E& e) { { e.entry() } -> std::same_as<std::string>; };

static_assert(TakesLiteral<aText> && TakesStdString<aText> && TakesEString<aText> && TakesResourceId<aText> && TakesCString<aText>);
static_assert(TakesLiteral<aTextListItem> && TakesStdString<aTextListItem> && TakesResourceId<aTextListItem> && TakesCString<aTextListItem>);
static_assert(EditTakesLiteral<aEdit> && EditTakesStdString<aEdit> && EditTakesEString<aEdit> && EditReturnsStdString<aEdit>);
} // namespace

TEST_CASE("widget text bridge: every caller shape resolves to one overload")
{
	CHECK(TakesLiteral<aText>);                // asserted at compile time above; this case keeps the file in the doctest report
}
