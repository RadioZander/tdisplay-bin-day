// Bin collection calendar from Horsham District Council's "Personalised Bin
// Calendar" page.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Bins collected on a day, as flags. The council marks each bin with a
// coloured bullet, which is what the parser goes by.
enum {
    BIN_REFUSE    = 1 << 0, // green bin
    BIN_RECYCLING = 1 << 1, // blue-top bin
    BIN_FOOD      = 1 << 2, // orange-top caddy
    BIN_GARDEN    = 1 << 3, // brown bin (garden waste subscription)
    BIN_OTHER     = 1 << 4, // a bullet colour we don't recognise
};

typedef struct {
    uint16_t year;
    uint8_t month; // 1-12
    uint8_t day;   // 1-31
    uint8_t bins;  // BIN_* flags
    bool changed;  // shown in bold: a change to the normal arrangements
} bin_collection_t;

#define BIN_CALENDAR_MAX 16

typedef struct {
    int64_t updated; // time of the last successful download, 0 if never
    int count;
    bin_collection_t collections[BIN_CALENDAR_MAX]; // in date order
} bin_calendar_t;

// Parse the calendar page's HTML. Keeps the `updated` time and returns the
// number of collections found (0 if the page has none, or its layout changed).
int bin_calendar_parse(const char *html, bin_calendar_t *cal);

// Parse the next piece of the page as it downloads, adding to `cal` (which
// starts with a count of 0). Returns how many characters were used: the
// rest is the start of a row that's been cut off, to go again with the
// next piece.
size_t bin_calendar_parse_more(const char *html, bin_calendar_t *cal);

// Called for each address on the postcode search page
typedef void (*bin_address_cb_t)(const char *uprn, const char *address, void *ctx);

#define BIN_ADDRESS_MAX_LEN 160

// Parse the postcode search page's HTML, calling `found` for each address.
// Returns the number of addresses.
int bin_address_parse(const char *html, bin_address_cb_t found, void *ctx);

// As bin_calendar_parse_more, for the postcode search page: calls `found`
// for each complete address and adds them to `*count`
size_t bin_address_parse_more(const char *html, bin_address_cb_t found, void *ctx, int *count);

// Reads the next part of a page into `buf`, up to `size` bytes. Returns the
// number read, 0 at the end, or -1 on an error.
typedef int (*bin_read_fn)(char *buf, int size, void *ctx);

// Parses the text so far and returns how many characters it used, like
// bin_calendar_parse_more
typedef size_t (*bin_parse_fn)(const char *text, void *ctx);

// Parse a page piece by piece as it's read, using `buf` (`size` bytes) to
// hold each piece. Needs far less memory than reading the whole page first.
// Returns false if reading fails.
bool bin_stream_parse(char *buf, size_t size, bin_read_fn read, void *read_ctx, bin_parse_fn parse, void *parse_ctx);

// The first collection on or after `today`, or NULL if there isn't one
const bin_collection_t *bin_calendar_next(const bin_calendar_t *cal, const struct tm *today);

// The collection's date as a number, yyyymmdd
uint32_t bin_collection_date_key(const bin_collection_t *c);

// Whole days from `today` to the collection: 0 = today, 1 = tomorrow
int bin_collection_days_until(const bin_collection_t *c, const struct tm *today);

// Download the calendar for a property, over HTTPS if the council's server
// allows it, otherwise plain HTTP. Needs WiFi and the correct time (for the
// HTTPS certificate check). Returns 0 on success.
int bin_calendar_fetch(const char *uprn, bin_calendar_t *cal);

// Look up the addresses for a postcode, calling `found` for each. Needs WiFi.
// Returns the number of addresses, or -1 if the lookup failed.
int bin_address_lookup(const char *postcode, bin_address_cb_t found, void *ctx);

// "HTTPS" or "HTTP" for the last successful download, or NULL if none yet
const char *bin_calendar_protocol(void);

// Keep a copy of the calendar in NVS, so it survives power cuts and outages
// of the council's website
void bin_calendar_save(const bin_calendar_t *cal);
bool bin_calendar_load(bin_calendar_t *cal);
