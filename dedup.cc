#include <stdio.h>
#include <ts/ts.h>

#include <string.h>
#include <HttpCompat.h>

typedef struct {

  TSHttpTxn txnp;
  TSMBuffer bufp;
  TSMLoc hdr_loc;

  TSMLoc location_loc;

  TSMLoc url_loc;
  TSCacheKey key;

  TSMLoc link_loc;

  int idx;

} Info;

static int
link_handler(TSCont contp, TSEvent event, void *edata)
{
  Info *info = (Info*) TSContDataGet(contp);

  const char *value;
  int length;

  switch (event) {
  case TS_EVENT_CACHE_OPEN_READ:
    TSHandleMLocRelease(info->bufp, info->hdr_loc, info->link_loc);

    value = TSUrlStringGet(info->bufp, info->url_loc, &length);

    TSMimeHdrFieldValuesClear(info->bufp, info->hdr_loc, info->location_loc);
    TSMimeHdrFieldValueStringInsert(info->bufp, info->hdr_loc, info->location_loc, -1, value, length);

    break;

  case TS_EVENT_CACHE_OPEN_READ_FAILED:
    TSMLoc next_loc;

    int count;

    info->idx++;
    do {

      count = TSMimeHdrFieldValuesCount(info->bufp, info->hdr_loc, info->link_loc);
      for (; info->idx < count; info->idx++) {
        value = TSMimeHdrFieldValueStringGet(info->bufp, info->hdr_loc, info->link_loc, info->idx, &length);

        if (!HttpCompat::lookup_param_in_semicolon_string(value, length, const_cast<char*>("rel"), const_cast<char*>("duplicate"), 9)) {
          continue;
        }

        if (TSUrlParse(info->bufp, info->url_loc, &value, value + length) != TS_PARSE_DONE) {
          continue;
        }

        if (TSCacheKeyDigestFromUrlSet(info->key, info->url_loc) != TS_SUCCESS) {
          continue;
        }

        TSCacheRead(contp, info->key);

        return 0;
      }

      next_loc = TSMimeHdrFieldNextDup(info->bufp, info->hdr_loc, info->link_loc);

      TSHandleMLocRelease(info->bufp, info->hdr_loc, info->link_loc);

      info->link_loc = next_loc;
      info->idx = 0;

    } while (info->link_loc);

    break;

  default:
    TSAssert(!"Unexpected event");
  }

  TSCacheKeyDestroy(info->key);

  TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->url_loc);
  TSHandleMLocRelease(info->bufp, info->hdr_loc, info->location_loc);
  TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->hdr_loc);

  TSHttpTxnReenable(info->txnp, TS_EVENT_HTTP_CONTINUE);

  TSfree(info);
  TSContDestroy(contp);

  return 0;
}

static int
location_handler(TSCont contp, TSEvent event, void *edata)
{
  Info *info = (Info*) TSContDataGet(contp);
  TSContDestroy(contp);

  switch (event) {
  case TS_EVENT_CACHE_OPEN_READ:
    break;

  case TS_EVENT_CACHE_OPEN_READ_FAILED:

    const char *value;
    int length;

    value = TSMimeHdrFieldValueStringGet(info->bufp, info->hdr_loc, info->link_loc, info->idx, &length);

    if (TSUrlParse(info->bufp, info->url_loc, &value, value + length) != TS_PARSE_DONE) {
      break;
    }

    if (TSCacheKeyDigestFromUrlSet(info->key, info->url_loc) != TS_SUCCESS) {
      break;
    }

    contp = TSContCreate(link_handler, NULL);
    TSContDataSet(contp, info);
    TSCacheRead(contp, info->key);

    return 0;

  default:
    TSAssert(!"Unexpected event");
  }

  TSHandleMLocRelease(info->bufp, info->hdr_loc, info->link_loc);

  TSCacheKeyDestroy(info->key);

  TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->url_loc);
  TSHandleMLocRelease(info->bufp, info->hdr_loc, info->location_loc);
  TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->hdr_loc);

  TSHttpTxnReenable(info->txnp, TS_EVENT_HTTP_CONTINUE);

  TSfree(info);

  return 0;
}

static int
handler(TSCont contp, TSEvent event, void *edata)
{
  Info *info = (Info*) TSmalloc(sizeof(Info));
  info->txnp = (TSHttpTxn) edata;

  switch (event) {
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:

    const char *value;
    int length;

    TSMLoc next_loc;

    int count;

    if (TSHttpTxnClientRespGet(info->txnp, &info->bufp, &info->hdr_loc) != TS_SUCCESS) {
      TSError("Couldn't retrieve client request header\n");

      break;
    }

    /* Assumption: Want to minimize cache read, so check first that:
     *
     *   1. response has "Location: ..." header
     *   2. response has RFC 6249 "Link: <...>; rel=duplicate" header
     *
     * Then scan if URL already exist in cache */

    /* If response has "Location: ..." header */
    info->location_loc = TSMimeHdrFieldFind(info->bufp, info->hdr_loc, TS_MIME_FIELD_LOCATION, TS_MIME_LEN_LOCATION);
    if (!info->location_loc) {
      TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->hdr_loc);

      break;
    }

    value = TSMimeHdrFieldValueStringGet(info->bufp, info->hdr_loc, info->location_loc, 0, &length);

    /* If can't parse or lookup "Location: ..." URL, should still check if
     * response has RFC 6249 "Link: <...>; rel=duplicate" header? No: Can't
     * parse or lookup URL in "Location: ..." header is error */
    TSUrlCreate(info->bufp, &info->url_loc);
    if (TSUrlParse(info->bufp, info->url_loc, &value, value + length) != TS_PARSE_DONE) {

      TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->url_loc);
      TSHandleMLocRelease(info->bufp, info->hdr_loc, info->location_loc);
      TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->hdr_loc);

      break;
    }

    info->key = TSCacheKeyCreate();
    if (TSCacheKeyDigestFromUrlSet(info->key, info->url_loc) != TS_SUCCESS) {
      TSCacheKeyDestroy(info->key);

      TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->url_loc);
      TSHandleMLocRelease(info->bufp, info->hdr_loc, info->location_loc);
      TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->hdr_loc);

      break;
    }

    /* ... and RFC 6249 "Link: <...>; rel=duplicate" header */
    info->link_loc = TSMimeHdrFieldFind(info->bufp, info->hdr_loc, "Link", 4);
    while (info->link_loc) {

      count = TSMimeHdrFieldValuesCount(info->bufp, info->hdr_loc, info->link_loc);
      for (info->idx = 0; info->idx < count; info->idx++) {

        value = TSMimeHdrFieldValueStringGet(info->bufp, info->hdr_loc, info->link_loc, info->idx, &length);
        if (!HttpCompat::lookup_param_in_semicolon_string(value, length, const_cast<char*>("rel"), const_cast<char*>("duplicate"), 9)) {
          continue;
        }

        contp = TSContCreate(location_handler, NULL);
        TSContDataSet(contp, info);
        TSCacheRead(contp, info->key);

        return 0;
      }

      next_loc = TSMimeHdrFieldNextDup(info->bufp, info->hdr_loc, info->link_loc);

      TSHandleMLocRelease(info->bufp, info->hdr_loc, info->link_loc);

      info->link_loc = next_loc;
    }

    TSCacheKeyDestroy(info->key);

    TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->url_loc);
    TSHandleMLocRelease(info->bufp, info->hdr_loc, info->location_loc);
    TSHandleMLocRelease(info->bufp, TS_NULL_MLOC, info->hdr_loc);

    break;

  default:
    TSAssert(!"Unexpected event");
  }

  TSHttpTxnReenable(info->txnp, TS_EVENT_HTTP_CONTINUE);

  TSfree(info);

  return 0;
}

void
TSPluginInit(int argc, const char *argv[])
{
  TSCont contp;
  TSPluginRegistrationInfo info;

  info.plugin_name = const_cast<char*>("dedup");
  info.vendor_name = const_cast<char*>("Jack Bates");
  info.support_email = const_cast<char*>("jack@nottheoilrig.com");

  if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
    TSError("Plugin registration failed\n");
  }

  contp = TSContCreate(handler, NULL);
  TSHttpHookAdd(TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);
}
