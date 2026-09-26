// Host test for the calendar parser. Run from the project root:
//   cc -Wall -Imain test/test_bin_calendar.c main/bin_calendar.c -lm -o test_bin_calendar && ./test_bin_calendar
// cal_details_sample.html is the part of the council's calendar page the
// parser reads, with the address replaced.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

typedef struct {
    int count;
    char uprns[4][16];
    char addresses[4][BIN_ADDRESS_MAX_LEN];
} found_addresses_t;

static void on_address(const char *uprn, const char *address, void *ctx)
{
    found_addresses_t *f = ctx;
    assert(f->count < 4);
    strcpy(f->uprns[f->count], uprn);
    strcpy(f->addresses[f->count], address);
    f->count++;
}

static void test_addresses(void)
{
    // The layout of the postcode search page (cal2.asp), with made-up addresses
    const char *html =
        "<select name=\"uprn\" id=\"uprn\">\r\n"
        "\t\t\t  <option value=\"\" selected=\"selected\" disabled=\"disabled\">Please select address...</option>\r\n"
        "\t\t\t              <option value='010000000001'>\r\n"
        "\t\t\t                1 Example Road Horsham West Sussex RH12 0XX \r\n"
        "              </option>\r\n"
        "\t\t\t              <option value='200000000002'>\r\n"
        "\t\t\t                Flat 2 Smith &amp; Sons   House Horsham RH12 0XX \r\n"
        "              </option>\r\n"
        "\t\t\t              <option value=\"100000000003\">3 O&#39;Brien Close</option>\r\n"
        "</select>";
    found_addresses_t f = {0};
    assert(bin_address_parse(html, on_address, &f) == 3);
    assert(strcmp(f.uprns[0], "010000000001") == 0);
    assert(strcmp(f.addresses[0], "1 Example Road Horsham West Sussex RH12 0XX") == 0);
    assert(strcmp(f.uprns[1], "200000000002") == 0);
    assert(strcmp(f.addresses[1], "Flat 2 Smith & Sons House Horsham RH12 0XX") == 0);
    assert(strcmp(f.uprns[2], "100000000003") == 0);
    assert(strcmp(f.addresses[2], "3 O'Brien Close") == 0);

    f.count = 0;
    assert(bin_address_parse("<p>No addresses found</p>", on_address, &f) == 0);
}

int main(void)
{
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    test_sample_page();
    test_changed_and_unknown();
    test_no_collections();
    test_next_and_days_until();
    test_addresses();
    printf("All bin calendar tests passed\n");
    return 0;
}
