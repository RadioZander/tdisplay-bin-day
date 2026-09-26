// Parsing and date handling for the bin calendar. Plain C with no ESP-IDF
// dependencies, so it can be tested on a PC (see test/).
//
// Each collection is a table row like this (whitespace added):
//   <tr bgcolor='#f1f2f2'>
//     <td class='ApplicationDetail'>Thursday</td>
//     <td class='ApplicationDetail'>24/09/2026</td>
//     <td class='ApplicationDetail'><ul class='accessible-bullets'>
//       <li><span aria-hidden='true' class='bullet green'></span><span class='text'>Green Bin for Refuse and Non-Recycling</span>&nbsp;</li>
//       <li><span aria-hidden='true' class='bullet orange'></span><span class='text'>Orange-Top Bin for Food Waste</span></li>
//     </ul></td>
//   </tr>
// A collection that differs from the normal arrangements is in bold.
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "bin_calendar.h"

static const struct {
    const char *colour;
    uint8_t bin;
} bullet_colours[] = {
    {"green", BIN_REFUSE},
    {"blue", BIN_RECYCLING},
    {"orange", BIN_FOOD},
    {"brown", BIN_GARDEN},
};

// strstr, but only matches that start before `end`
static const char *find_before(const char *start, const char *end, const char *needle)
{
    const char *p = strstr(start, needle);
    return p && p < end ? p : NULL;
}

// Reads a dd/mm/yyyy date at `s`
static bool parse_date(const char *s, bin_collection_t *c)
{
    for (int i = 0; i < 10; i++) {
        if (i == 2 || i == 5 ? s[i] != '/' : !isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    int day = (s[0] - '0') * 10 + (s[1] - '0');
    int month = (s[3] - '0') * 10 + (s[4] - '0');
    int year = atoi(s + 6);
    if (day < 1 || day > 31 || month < 1 || month > 12) {
        return false;
    }
    c->day = day;
    c->month = month;
    c->year = year;
    return true;
}

// The bin for the colour word at `s`, e.g. "green'>"
static uint8_t bin_from_colour(const char *s)
{
    size_t len = strspn(s, "abcdefghijklmnopqrstuvwxyz");
    for (size_t i = 0; i < sizeof(bullet_colours) / sizeof(bullet_colours[0]); i++) {
        if (strlen(bullet_colours[i].colour) == len && strncmp(s, bullet_colours[i].colour, len) == 0) {
            return bullet_colours[i].bin;
        }
    }
    return BIN_OTHER;
}

static int date_key(const bin_collection_t *c)
{
    return c->year * 10000 + c->month * 100 + c->day;
}

static int compare_dates(const void *a, const void *b)
{
    return date_key(a) - date_key(b);
}

int bin_calendar_parse(const char *html, bin_calendar_t *cal)
{
    cal->count = 0;
    for (const char *row = strstr(html, "<tr"); row && cal->count < BIN_CALENDAR_MAX; row = strstr(row + 3, "<tr")) {
        const char *end = strstr(row, "</tr>");
        if (!end) {
            break;
        }

        // Rows without a date are headings
        bin_collection_t c = {0};
        bool dated = false;
        for (const char *p = row; p + 10 <= end && !dated; p++) {
            dated = parse_date(p, &c);
        }
        if (!dated) {
            continue;
        }

        for (const char *p = row; (p = find_before(p, end, "bullet ")); p += 7) {
            c.bins |= bin_from_colour(p + 7);
        }
        // A date with no bullets means the page layout has changed, but the
        // date is still worth showing
        if (!c.bins) {
            c.bins = BIN_OTHER;
        }
        c.changed = find_before(row, end, "<strong") || find_before(row, end, "<b>") || find_before(row, end, "<b ");
        cal->collections[cal->count++] = c;
    }
    qsort(cal->collections, cal->count, sizeof(cal->collections[0]), compare_dates);
    return cal->count;
}

const bin_collection_t *bin_calendar_next(const bin_calendar_t *cal, const struct tm *today)
{
    int today_key = (today->tm_year + 1900) * 10000 + (today->tm_mon + 1) * 100 + today->tm_mday;
    for (int i = 0; i < cal->count; i++) {
        if (date_key(&cal->collections[i]) >= today_key) {
            return &cal->collections[i];
        }
    }
    return NULL;
}

int bin_collection_days_until(const bin_collection_t *c, const struct tm *today)
{
    // Compare the two days at midday, so a clock change in between can't
    // push the difference over a whole day
    struct tm from = {.tm_year = today->tm_year, .tm_mon = today->tm_mon, .tm_mday = today->tm_mday,
                      .tm_hour = 12, .tm_isdst = -1};
    struct tm to = {.tm_year = c->year - 1900, .tm_mon = c->month - 1, .tm_mday = c->day,
                    .tm_hour = 12, .tm_isdst = -1};
    return (int)lround(difftime(mktime(&to), mktime(&from)) / 86400.0);
}
