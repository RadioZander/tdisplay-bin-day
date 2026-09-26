// Host test for the calendar parser. Run from the project root:
//   cc -Wall -Imain test/test_bin_calendar.c main/bin_calendar.c -lm -o test_bin_calendar && ./test_bin_calendar
// cal_details_sample.html is a saved copy of the council's page with the
// address replaced.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "bin_calendar.h"

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static struct tm day(int year, int month, int mday)
{
    return (struct tm){.tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = mday};
}

static void test_sample_page(void)
{
    char *html = read_file("test/cal_details_sample.html");
    bin_calendar_t cal = {.updated = 1234};
    assert(bin_calendar_parse(html, &cal) == 10);
    assert(cal.updated == 1234);

    const bin_collection_t *c = &cal.collections[0];
    assert(c->year == 2026 && c->month == 9 && c->day == 24);
    assert(c->bins == (BIN_REFUSE | BIN_FOOD));
    assert(!c->changed);

    c = &cal.collections[1];
    assert(c->year == 2026 && c->month == 10 && c->day == 1);
    assert(c->bins == (BIN_RECYCLING | BIN_FOOD));

    c = &cal.collections[9];
    assert(c->month == 11 && c->day == 26);
    free(html);
}

static void test_changed_and_unknown(void)
{
    // A bold row with an unknown bullet colour, and an out-of-order row
    const char *html =
        "<table><thead><tr><th>DAY</th><th>DATE</th></tr></thead>"
        "<tr><td>Saturday</td><td><strong>27/12/2025</strong></td><td>"
        "<span class='bullet purple'></span><span class='bullet brown'></span></td></tr>"
        "<tr><td>Monday</td><td>22/12/2025</td><td>No bullets here</td></tr>"
        "</table>";
    bin_calendar_t cal = {0};
    assert(bin_calendar_parse(html, &cal) == 2);
    assert(cal.collections[0].day == 22 && cal.collections[0].bins == BIN_OTHER && !cal.collections[0].changed);
    assert(cal.collections[1].day == 27 && cal.collections[1].bins == (BIN_OTHER | BIN_GARDEN));
    assert(cal.collections[1].changed);
}

static void test_no_collections(void)
{
    bin_calendar_t cal = {0};
    assert(bin_calendar_parse("<html>No collection information</html>", &cal) == 0);
    struct tm today = day(2026, 9, 26);
    assert(bin_calendar_next(&cal, &today) == NULL);
}

static void test_next_and_days_until(void)
{
    char *html = read_file("test/cal_details_sample.html");
    bin_calendar_t cal = {0};
    bin_calendar_parse(html, &cal);

    struct tm today = day(2026, 9, 24);
    const bin_collection_t *c = bin_calendar_next(&cal, &today);
    assert(c->day == 24 && bin_collection_days_until(c, &today) == 0);

    today = day(2026, 9, 26);
    c = bin_calendar_next(&cal, &today);
    assert(c->month == 10 && c->day == 1);
    assert(bin_collection_days_until(c, &today) == 5);

    // Across the end of British Summer Time (25 Oct 2026)
    today = day(2026, 10, 21);
    c = bin_calendar_next(&cal, &today);
    assert(c->day == 22 && bin_collection_days_until(c, &today) == 1);
    today = day(2026, 10, 23);
    c = bin_calendar_next(&cal, &today);
    assert(c->day == 29 && bin_collection_days_until(c, &today) == 6);

    today = day(2026, 11, 27);
    assert(bin_calendar_next(&cal, &today) == NULL);
    free(html);
}

int main(void)
{
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    test_sample_page();
    test_changed_and_unknown();
    test_no_collections();
    test_next_and_days_until();
    printf("All bin calendar tests passed\n");
    return 0;
}
