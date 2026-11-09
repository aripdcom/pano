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

#include <string.h>

// The letters for the labels, the easiest to type first
#define LABEL_LETTERS "asdfjklghqweruiopzxcvbnmty"
#define N_LETTERS (sizeof(LABEL_LETTERS) - 1)

/* The built-in patterns, in order of precedence: a match of an earlier
 * pattern wins over overlapping matches of later ones. */
static struct {
  char const* pattern;
  bool openable;
} const builtin_patterns[] = {
  // URLs; trailing punctuation is trimmed below
  { "\\b(?:https?|ftp|file|ssh|git)://[^\\s<>\"'`]+", true },
  // UUIDs
  { "\\b[[:xdigit:]]{8}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{12}\\b", false },
  // IPv4 addresses, with an optional port
  { "\\b(?:\\d{1,3}\\.){3}\\d{1,3}(?::\\d+)?\\b", false },
  // IPv6 addresses with at least three groups, some maybe empty ("::")
  { "(?<![:\\w])(?:[[:xdigit:]]{0,4}:){2,7}[[:xdigit:]]{1,4}(?![:\\w])", false },
  // Paths: absolute, from home or relative with a slash
  { "(?:~|\\.\\.?)?/[\\w.@%+~-][\\w./@%+~:-]*|(?<![\\w/])[\\w.-]+(?:/[\\w.@%+~-]+)+/?", true },
  // Git hashes and other hex strings with letters and digits
  { "\\b(?=[[:xdigit:]]*[a-f])(?=[[:xdigit:]]*\\d)[[:xdigit:]]{7,64}\\b", false },
  // Numbers with four or more digits, like process IDs and ports
  { "(?<![\\w.])\\d{4,}(?![\\w.])", false },
};

static void
match_clear(void* data)
{
  auto const match = reinterpret_cast<TerminalQuickSelectMatch*>(data);
  g_free(match->text);
}

/* The column at the byte @offset of @line: wide characters take two
 * columns, combining ones none */
static long
column_at(char const* line,
          long offset)
{
  long column = 0;
  for (auto p = line; *p && p < line + offset; p = g_utf8_next_char(p)) {
    auto const c = g_utf8_get_char(p);
    if (g_unichar_iszerowidth(c))
      continue;
    column += g_unichar_iswide(c) ? 2 : 1;
  }
  return column;
}

static bool
overlaps(GArray* matches,
         guint line,
         long start_col,
         long end_col)
{
  for (auto i = 0u; i < matches->len; ++i) {
    auto const& match = g_array_index(matches, TerminalQuickSelectMatch, i);
    if (match.line == line && start_col < match.end_col && match.start_col < end_col)
      return true;
  }
  return false;
}

/* Drops punctuation that ends a sentence, and closing brackets without an
 * opening one in the match, as in "(see https://example.org)." */
static int
trim_url_end(char const* text,
             int start,
             int end)
{
  while (end > start) {
    auto const c = text[end - 1];
    if (strchr(".,;:!?'\"", c)) {
      --end;
      continue;
    }

    char open = 0;
    if (c == ')')
      open = '(';
    else if (c == ']')
      open = '[';
    else if (c == '}')
      open = '{';
    if (!open)
      break;

    auto balance = 0;
    for (auto i = start; i < end; ++i) {
      if (text[i] == open)
        ++balance;
      else if (text[i] == c)
        --balance;
    }
    if (balance >= 0)
      break;
    --end;
  }
  return end;
}

static void
find_pattern(GArray* matches,
             char const* const* lines,
             GRegex* regex,
             bool openable,
             bool is_url)
{
  for (auto n = 0u; lines[n]; ++n) {
    auto const line = lines[n];
    GMatchInfo* info = nullptr;
    g_regex_match(regex, line, GRegexMatchFlags(0), &info);
    while (g_match_info_matches(info)) {
      int start = 0, end = 0;
      g_match_info_fetch_pos(info, 0, &start, &end);
      if (is_url)
        end = trim_url_end(line, start, end);

      auto const start_col = column_at(line, start);
      auto const end_col = column_at(line, end);
      if (end > start && !overlaps(matches, n, start_col, end_col) &&
          matches->len < TERMINAL_QUICK_SELECT_MAX_MATCHES) {
        TerminalQuickSelectMatch match{};
        match.line = n;
        match.start_col = start_col;
        match.end_col = end_col;
        match.text = g_strndup(line + start, end - start);
        match.openable = openable;
        g_array_append_val(matches, match);
      }
      g_match_info_next(info, nullptr);
    }
    g_match_info_free(info);
  }
}

/**
 * terminal_quick_select_find:
 * @lines: (array zero-terminated=1): the lines of text, one per row
 * @user_patterns: (array zero-terminated=1) (nullable): regular
 *   expressions of the user, which take precedence
 *
 * Returns: (transfer full) (element-type TerminalQuickSelectMatch): the
 *   matches, without labels yet
 */
GArray*
terminal_quick_select_find(char const* const* lines,
                           char const* const* user_patterns)
{
  auto const matches = g_array_new(false, true, sizeof(TerminalQuickSelectMatch));
  g_array_set_clear_func(matches, match_clear);

  for (auto p = user_patterns; p && *p; ++p) {
    GError* error = nullptr;
    auto const regex = g_regex_new(*p, G_REGEX_OPTIMIZE, GRegexMatchFlags(0), &error);
    if (!regex) {
      g_warning("Ignoring the quick select pattern “%s”: %s", *p, error->message);
      g_error_free(error);
      continue;
    }
    find_pattern(matches, lines, regex, false, false);
    g_regex_unref(regex);
  }

  static GRegex* regexes[G_N_ELEMENTS(builtin_patterns)];
  for (auto i = 0u; i < G_N_ELEMENTS(builtin_patterns); ++i) {
    if (!regexes[i])
      regexes[i] = g_regex_new(builtin_patterns[i].pattern, G_REGEX_OPTIMIZE,
                               GRegexMatchFlags(0), nullptr);
    if (regexes[i])
      find_pattern(matches, lines, regexes[i], builtin_patterns[i].openable, i == 0);
  }

  return matches;
}

static int
compare_position(void const* a,
                 void const* b)
{
  auto const ma = reinterpret_cast<TerminalQuickSelectMatch const*>(a);
  auto const mb = reinterpret_cast<TerminalQuickSelectMatch const*>(b);
  // The bottom first, closest to the prompt, then from the left
  if (ma->line != mb->line)
    return ma->line > mb->line ? -1 : 1;
  return ma->start_col < mb->start_col ? -1 : ma->start_col > mb->start_col ? 1 : 0;
}

/**
 * terminal_quick_select_assign_labels:
 * @matches: the matches
 *
 * Gives each match a label, the ones near the bottom first: one letter
 * when there are few matches, else two letters, so that no label is the
 * start of another.
 */
void
terminal_quick_select_assign_labels(GArray* matches)
{
  g_array_sort(matches, compare_position);

  auto const two = matches->len > N_LETTERS;
  for (auto i = 0u; i < matches->len; ++i) {
    auto& match = g_array_index(matches, TerminalQuickSelectMatch, i);
    if (two) {
      match.label[0] = LABEL_LETTERS[i / N_LETTERS];
      match.label[1] = LABEL_LETTERS[i % N_LETTERS];
      match.label[2] = '\0';
    } else {
      match.label[0] = LABEL_LETTERS[i];
      match.label[1] = '\0';
    }
  }
}

/**
 * terminal_quick_select_lookup:
 * @matches: the labelled matches
 * @typed: the letters typed so far
 * @is_prefix: (out): whether @typed starts some labels without being one
 *
 * Returns: (nullable): the match whose label is @typed
 */
TerminalQuickSelectMatch const*
terminal_quick_select_lookup(GArray* matches,
                             char const* typed,
                             gboolean* is_prefix)
{
  *is_prefix = false;
  for (auto i = 0u; i < matches->len; ++i) {
    auto const& match = g_array_index(matches, TerminalQuickSelectMatch, i);
    if (g_str_equal(match.label, typed))
      return &match;
    if (g_str_has_prefix(match.label, typed))
      *is_prefix = true;
  }
  return nullptr;
}
