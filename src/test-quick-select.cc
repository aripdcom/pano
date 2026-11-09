/*
 * Copyright © 2026 aripdcem
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "terminal-quick-select.hh"

static GArray*
find(char const* const* lines,
     char const* const* user_patterns = nullptr)
{
  return terminal_quick_select_find(lines, user_patterns);
}

static void
assert_texts(GArray* matches,
             char const* const* expected)
{
  auto i = 0u;
  for (; expected[i]; ++i) {
    g_assert_cmpuint(i, <, matches->len);
    g_assert_cmpstr(g_array_index(matches, TerminalQuickSelectMatch, i).text, ==, expected[i]);
  }
  g_assert_cmpuint(i, ==, matches->len);
}

static void
test_kinds(void)
{
  char const* const lines[] = {
    "see https://example.org/a_(b)?q=1, or (https://gnome.org).",
    "commit 3c75cf2d1e and 9cf149c1 by pid 48213",
    "connect to 192.168.1.20:8080 or fe80::1:2:3",
    "edit ~/src/main.cc and src/terminal-tab.cc in /usr/share/pano-terminal/",
    "id 1b4e28ba-2fa1-11d2-883f-0016d3cca427 at 12:30 in 2026",
    nullptr,
  };
  g_autoptr(GArray) matches = find(lines);

  // In the order of the patterns, then of the lines
  char const* const expected[] = {
    "https://example.org/a_(b)?q=1",
    "https://gnome.org",
    "1b4e28ba-2fa1-11d2-883f-0016d3cca427",
    "192.168.1.20:8080",
    "fe80::1:2:3",
    "~/src/main.cc",
    "src/terminal-tab.cc",
    "/usr/share/pano-terminal/",
    "3c75cf2d1e",
    "9cf149c1",
    "48213",
    "2026",
    nullptr,
  };
  assert_texts(matches, expected);

  auto const& url = g_array_index(matches, TerminalQuickSelectMatch, 0);
  g_assert_cmpuint(url.line, ==, 0);
  g_assert_cmpint(url.start_col, ==, 4);
  g_assert_cmpint(url.end_col, ==, 4 + long(strlen("https://example.org/a_(b)?q=1")));
  g_assert_true(url.openable);
}

static void
test_columns(void)
{
  // Wide characters take two columns
  char const* const lines[] = { "日本 https://x.org", nullptr };
  g_autoptr(GArray) matches = find(lines);
  g_assert_cmpuint(matches->len, ==, 1);
  g_assert_cmpint(g_array_index(matches, TerminalQuickSelectMatch, 0).start_col, ==, 5);
}

static void
test_user_patterns(void)
{
  char const* const lines[] = { "ticket PANO-123 and https://x.org/PANO-9", nullptr };
  char const* const patterns[] = { "PANO-\\d+", "(", nullptr };
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*Ignoring the quick select pattern*");
  g_autoptr(GArray) matches = find(lines, patterns);
  g_test_assert_expected_messages();

  // The user's patterns win over the URL around the second match
  char const* const expected[] = { "PANO-123", "PANO-9", nullptr };
  assert_texts(matches, expected);
}

static void
test_labels(void)
{
  char const* const lines[] = { "1111 2222", "3333", nullptr };
  g_autoptr(GArray) matches = find(lines);
  terminal_quick_select_assign_labels(matches);

  // The bottom first
  g_assert_cmpstr(g_array_index(matches, TerminalQuickSelectMatch, 0).text, ==, "3333");
  g_assert_cmpstr(g_array_index(matches, TerminalQuickSelectMatch, 0).label, ==, "a");
  g_assert_cmpstr(g_array_index(matches, TerminalQuickSelectMatch, 1).label, ==, "s");

  gboolean is_prefix;
  auto const match = terminal_quick_select_lookup(matches, "s", &is_prefix);
  g_assert_nonnull(match);
  g_assert_cmpstr(match->text, ==, "1111");
  g_assert_null(terminal_quick_select_lookup(matches, "x", &is_prefix));
  g_assert_false(is_prefix);

  // Many matches get two letters, and one letter is only a prefix
  GString* line = g_string_new(nullptr);
  for (auto i = 0; i < 40; ++i)
    g_string_append_printf(line, "%d ", 1000 + i);
  char const* const many[] = { line->str, nullptr };
  g_autoptr(GArray) more = find(many);
  terminal_quick_select_assign_labels(more);
  g_assert_cmpuint(more->len, ==, 40);
  for (auto i = 0u; i < more->len; ++i)
    g_assert_cmpuint(strlen(g_array_index(more, TerminalQuickSelectMatch, i).label), ==, 2);
  g_assert_null(terminal_quick_select_lookup(more, "a", &is_prefix));
  g_assert_true(is_prefix);
  g_string_free(line, true);
}

int
main(int argc,
     char* argv[])
{
  g_test_init(&argc, &argv, nullptr);

  g_test_add_func("/quick-select/kinds", test_kinds);
  g_test_add_func("/quick-select/columns", test_columns);
  g_test_add_func("/quick-select/user-patterns", test_user_patterns);
  g_test_add_func("/quick-select/labels", test_labels);

  return g_test_run();
}
