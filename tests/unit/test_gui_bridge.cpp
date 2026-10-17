// tests/unit/test_gui_bridge.cpp
//
// Widget text accepts literals, owned strings and views alongside resource IDs.

#include "doctest.h"

#include "aedit.h"
#include "alistbox.h"
#include "asystem.h"

#include <concepts>
#include <string>
#include <string_view>

namespace
{
template <class W> concept TakesLiteral = requires(W& w) { w.setText("x"); };
template <class W> concept TakesStdString = requires(W& w, const std::string& s) { w.setText(s); };
template <class W> concept TakesStringView = requires(W& w, std::string_view e) { w.setText(e); };
template <class W> concept TakesResourceId = requires(W& w) { w.setText(0L); };
template <class W> concept TakesCString = requires(W& w, const char* p) { w.setText(p); };

template <class E> concept EditTakesLiteral = requires(E& e) { e.setEntry("x"); };
template <class E> concept EditTakesStdString = requires(E& e, const std::string& s) { e.setEntry(s, 1); };
template <class E> concept EditTakesStringView = requires(E& e, std::string_view s) { e.setEntry(s); };
template <class E> concept EditReturnsStdString = requires(const E& e) { { e.entry() } -> std::same_as<std::string>; };

static_assert(TakesLiteral<aText> && TakesStdString<aText> && TakesStringView<aText> && TakesResourceId<aText> && TakesCString<aText>);
static_assert(TakesLiteral<aTextListItem> && TakesStdString<aTextListItem> && TakesStringView<aTextListItem> && TakesResourceId<aTextListItem> && TakesCString<aTextListItem>);
static_assert(EditTakesLiteral<aEdit> && EditTakesStdString<aEdit> && EditTakesStringView<aEdit> && EditReturnsStdString<aEdit>);
} // namespace

TEST_CASE("widget text: every caller shape resolves to one overload")
{
	CHECK(TakesLiteral<aText>);                // asserted at compile time above; this case keeps the file in the doctest report
}
