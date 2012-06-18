#include <stdio.h>
#include <string.h>

#include <ts/ts.h>

typedef struct {

  TSHttpTxn txnp;
  TSMBuffer bufp;
  TSMLoc hdr_loc;

  /* "Location: ..." header */
  TSMLoc location_loc;

  /* Cache key */
  TSMLoc url_loc;
  TSCacheKey key;

  /* RFC 6249 "Link: <...>; rel=duplicate" header */
  TSMLoc link_loc;

  /* Link header field value index */
  int idx;

} Info;

static bool
rel_duplicate(const char *value, const char *end)
{
  int length;

  while (value + 13 /* rel=duplicate */ < end) {

    value += 1;
    value += strspn(value, " ");

    if (strncmp(value, "rel=", 4)) {

      value = strchr(value, '=');
      if (!value) {
        return false;
      }

      value += 1;

      /* quoted-string */
      if (*value == '"') {

        for (value += 1;;) {

          value += strcspn(value, "\\\"");
          if (*value == '"') {
            value += 1;

            break; // continue 2;
          }

          value += 2;
          if (value + 15 /* ";rel=duplicate */ >= end) {
            return false;
          }
        }

      /* ptoken */
      } else {

        value = strchr(value, ';');
        if (!value) {
          return false;
        }
      }

      continue;
    }

    value += 4;

    /* <"> relation-type *( 1*SP relation-type ) <"> */
    if (*value == '"') {

      for (value += 1;;) {

        length = strcspn(value, " \"");
        if (strncmp(value, "duplicate", length)) {

          value += length;
          if (*value == '"') {
            value += 1;

            break; // continue 2;
          }

          value += 1;
          if (value + 15 /* ";rel=duplicate */ >= end) {
            return false;
          }
        }

        return true;
      }

    /* relation-type */
    } else {

      length = strcspn(value, ";");
      if (strncmp(value, "duplicate", length)) {
        value += 1;

        continue;
      }

      return true;
    }
  }

  return false;
}

/* Check if RFC 6249 "Link: <...>; rel=duplicate" URL already exist in cache */

static int
link_handler(TSCont contp, TSEvent event, void *edata)
{
  Info *info = (Info*) TSContDataGet(contp);

  const char *value;
  int length;

  switch (event) {

  /* Yes: Update "Location: ..." header and reenable response */
  case TS_EVENT_CACHE_OPEN_READ:
    TSHandleMLocRelease(info->bufp, info->hdr_loc, info->link_loc);

    value = TSUrlStringGet(info->bufp, info->url_loc, &length);

    TSMimeHdrFieldValuesClear(info->bufp, info->hdr_loc, info->location_loc);
    TSMimeHdrFieldValueStringInsert(info->bufp, info->hdr_loc, info->location_loc, -1, value, length);

    break;

  /* No: Check next RFC 6249 "Link: <...>; rel=duplicate" URL */
  case TS_EVENT_CACHE_OPEN_READ_FAILED:
    TSMLoc next_loc;

    int count;

    const char *start;
    const char *end;

    info->idx += 1;
    do {

      count = TSMimeHdrFieldValuesCount(info->bufp, info->hdr_loc, info->link_loc);
      for (; info->idx < count; info->idx += 1) {
        value = TSMimeHdrFieldValueStringGet(info->bufp, info->hdr_loc, info->link_loc, info->idx, &length);

        /* "string values returned from marshall buffers are not
         * null-terminated.  If you need a null-terminated value, then use
         * TSstrndup to automatically null-terminate a string",
         * http://trafficserver.apache.org/docs/trunk/sdk/http-headers/guide-to-trafficserver-http-header-system/index.en.html */
        value = TSstrndup(value, length);

        /* link-value = "<" URI-Reference ">" *( ";" link-param ) ; RFC 5988 */
        start = value + 1;

        end = strchr(start, '>');
        if (!end || !rel_duplicate(end + 1, value + length)
            || TSUrlParse(info->bufp, info->url_loc, &start, end) != TS_PARSE_DONE
            || TSCacheKeyDigestFromUrlSet(info->key, info->url_loc) != TS_SUCCESS) {
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

/* Check if "Location: ..." URL already exist in cache */

static int
location_handler(TSCont contp, TSEvent event, void *edata)
{
  Info *info = (Info*) TSContDataGet(contp);
  TSContDestroy(contp);

  switch (event) {

  /* Yes: Do nothing, just reenable response */
  case TS_EVENT_CACHE_OPEN_READ:
    break;

  /* No: Check RFC 6249 "Link: <...>; rel=duplicate" URL */
  case TS_EVENT_CACHE_OPEN_READ_FAILED:

    const char *value;
    int length;

    const char *start;
    const char *end;

    value = TSMimeHdrFieldValueStringGet(info->bufp, info->hdr_loc, info->link_loc, info->idx, &length);

    /* link-value = "<" URI-Reference ">" *( ";" link-param ) ; RFC 5988 */
    start = value + 1;

    /* memchr() vs. strchr() because "not null-terminated cannot be passed into
     * the common str*() routines",
     * http://trafficserver.apache.org/docs/trunk/sdk/http-headers/guide-to-trafficserver-http-header-system/index.en.html */
    end = (char*) memchr(start, '>', length - 1);
    if (!end
        || TSUrlParse(info->bufp, info->url_loc, &start, end) != TS_PARSE_DONE
        || TSCacheKeyDigestFromUrlSet(info->key, info->url_loc) != TS_SUCCESS) {
      contp = TSContCreate(link_handler, NULL);
      TSContDataSet(contp, info);

      link_handler(contp, TS_EVENT_CACHE_OPEN_READ_FAILED, NULL);

      return 0;
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

    const char *start;
    const char *end;

    if (TSHttpTxnClientRespGet(info->txnp, &info->bufp, &info->hdr_loc) != TS_SUCCESS) {
      TSError("Couldn't retrieve client response header");

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
      for (info->idx = 0; info->idx < count; info->idx += 1) {
        value = TSMimeHdrFieldValueStringGet(info->bufp, info->hdr_loc, info->link_loc, info->idx, &length);

        /* "string values returned from marshall buffers are not
         * null-terminated.  If you need a null-terminated value, then use
         * TSstrndup to automatically null-terminate a string",
         * http://trafficserver.apache.org/docs/trunk/sdk/http-headers/guide-to-trafficserver-http-header-system/index.en.html */
        value = TSstrndup(value, length);

        /* link-value = "<" URI-Reference ">" *( ";" link-param ) ; RFC 5988 */
        start = value + 1;

        end = strchr(start, '>');
        if (!end || !rel_duplicate(end + 1, value + length)) {
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

  info.plugin_name = const_cast<char*>("metalink");
  info.vendor_name = const_cast<char*>("Jack Bates");
  info.support_email = const_cast<char*>("jack@nottheoilrig.com");

  if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
    TSError("Plugin registration failed");
  }

  contp = TSContCreate(handler, NULL);
  TSHttpHookAdd(TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);
}
