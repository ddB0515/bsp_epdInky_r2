#include "trmnl_cache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "trmnl_cache";

#define CACHE_DIR       BSP_SD_MOUNT_POINT "/trmnl"
#define CACHE_TMP       CACHE_DIR "/.part"

/* Enough for a couple of days at the default cadence, and small enough that the
 * prune scan below stays cheap. Images are ~15 KB, so this is well under 1 MB. */
#define CACHE_MAX_FILES 64

/* The longest name we will build a path for. FATFS allows far more, but the
 * server's names are short and a bound keeps the path buffers on the stack. */
#define CACHE_NAME_MAX  96

static bool s_available;

/* Kept from init() so trmnl_cache_deinit() can cut the card's power again
 * without the caller having to hand the expander back. */
static tca6408_handle_t s_tca;

/* -------------------------------------------------------------------------- */

/*
 * Turn the server's `filename` into something safe to concatenate onto a path.
 *
 * Two separate jobs. The security one: the value comes off the network, so
 * anything that could climb out of the cache directory has to go - path
 * separators and "..". The practical one: FAT rejects a handful of characters
 * outright, and a name that fails to open would show up as a permanent cache
 * miss rather than an error anyone would notice.
 *
 * Rejecting rather than rewriting would be safer still, but the server picks
 * these names and a rewrite that is stable for a given input is enough: two
 * different server names can only collide if they differ solely in characters
 * we replace, which does not happen with the hashed names TRMNL actually sends.
 */
static bool sanitise(const char *in, char *out, size_t out_len)
{
    if (in == NULL || *in == '\0' || out_len == 0) {
        return false;
    }

    /* Keep only the last path component, so "../../x" and "/etc/x" both reduce
     * to "x" before anything else looks at them. */
    const char *base = in;
    for (const char *p = in; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    if (*base == '\0') {
        return false;
    }

    size_t n = 0;
    for (const char *p = base; *p != '\0' && n + 1 < out_len && n < CACHE_NAME_MAX; p++) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out[n++] = ok ? c : '_';
    }
    out[n] = '\0';

    /* A name of all dots would still address the directory itself. */
    if (strspn(out, ".") == n) {
        return false;
    }
    return n != 0;
}

static bool cache_path(const char *filename, char *out, size_t out_len)
{
    char safe[CACHE_NAME_MAX + 1];
    if (!sanitise(filename, safe, sizeof(safe))) {
        ESP_LOGW(TAG, "unusable filename from the server; not caching");
        return false;
    }
    return snprintf(out, out_len, CACHE_DIR "/%s", safe) < (int)out_len;
}

/* -------------------------------------------------------------------------- */

esp_err_t trmnl_cache_init(const bsp_epdinky_config_t *cfg, tca6408_handle_t tca)
{
    if (s_available) {
        return ESP_OK;
    }
    if (tca == NULL) {
        ESP_LOGW(TAG, "no expander handle; SD power cannot be switched");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = bsp_sdcard_mount(cfg, tca);
    if (err != ESP_OK) {
        /*
         * Expected whenever no card is fitted - there is no card-detect line, so
         * this is the only way to find out. Deliberately a warning, not an
         * error: the firmware works without it, just less efficiently.
         */
        ESP_LOGW(TAG, "no SD card (%s) - running uncached, every cycle will "
                      "download and do a full refresh", esp_err_to_name(err));
        return err;
    }

    if (mkdir(CACHE_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "cannot create %s: %s", CACHE_DIR, strerror(errno));
        bsp_sdcard_unmount(tca);
        return ESP_FAIL;
    }

    s_tca       = tca;
    s_available = true;
    ESP_LOGI(TAG, "cache ready at %s", CACHE_DIR);
    return ESP_OK;
}

void trmnl_cache_deinit(void)
{
    if (!s_available) {
        return;
    }

    /*
     * bsp_sdcard_unmount() flushes FATFS and then drops the card's supply
     * through the expander. Both halves matter before a deep sleep: an
     * unflushed FAT would corrupt the cache, and a powered card idles at a few
     * milliamps - which is the same order as everything else on this board put
     * together once the CPU is off.
     */
    const esp_err_t err = bsp_sdcard_unmount(s_tca);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unmount failed: %s", esp_err_to_name(err));
    }

    s_available = false;
    s_tca       = NULL;
}

bool trmnl_cache_available(void)
{
    return s_available;
}

bool trmnl_cache_has(const char *filename)
{
    char path[sizeof(CACHE_DIR) + CACHE_NAME_MAX + 2];
    if (!s_available || !cache_path(filename, path, sizeof(path))) {
        return false;
    }

    struct stat st;
    /* Zero-length files are the signature of an interrupted write on a
     * filesystem without the rename below; treat them as absent. */
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

esp_err_t trmnl_cache_read(const char *filename, uint8_t **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out     = NULL;
    *out_len = 0;

    char path[sizeof(CACHE_DIR) + CACHE_NAME_MAX + 2];
    if (!s_available || !cache_path(filename, path, sizeof(path))) {
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const size_t len = (size_t)st.st_size;
    uint8_t     *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = heap_caps_malloc(len, MALLOC_CAP_DEFAULT);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "out of memory for a %u byte cache entry", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    FILE  *f    = fopen(path, "rb");
    size_t got  = (f != NULL) ? fread(buf, 1, len, f) : 0;
    if (f != NULL) {
        fclose(f);
    }

    if (got != len) {
        /* Drop it rather than keep returning a broken entry: the next cycle
         * re-downloads and the cache repairs itself. */
        ESP_LOGW(TAG, "cache entry unreadable (%u of %u bytes); discarding",
                 (unsigned)got, (unsigned)len);
        unlink(path);
        heap_caps_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    *out     = buf;
    *out_len = len;
    ESP_LOGI(TAG, "cache hit: %s (%u bytes)", filename, (unsigned)len);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

/*
 * Keep the directory bounded. Deletes the single oldest entry per call, which is
 * enough because exactly one file is added per cycle, and avoids holding a large
 * sorted list on a device where the answer is nearly always "nothing to do".
 *
 * Ordering is by mtime, which is only meaningful because SNTP has set the clock
 * before any of this runs. Files written before the first successful sync all
 * carry the FAT epoch and are therefore pruned first - which is the right
 * outcome anyway, since they are the oldest.
 */
static void prune(void)
{
    DIR *dir = opendir(CACHE_DIR);
    if (dir == NULL) {
        return;
    }

    unsigned count = 0;
    char     oldest_name[CACHE_NAME_MAX + 1] = { 0 };
    time_t   oldest_time = 0;

    const struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;   /* skips ".", ".." and the .part temporary */
        }

        char path[sizeof(CACHE_DIR) + CACHE_NAME_MAX + 2];
        if (snprintf(path, sizeof(path), CACHE_DIR "/%s", ent->d_name) >= (int)sizeof(path)) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        count++;
        if (oldest_name[0] == '\0' || st.st_mtime < oldest_time) {
            oldest_time = st.st_mtime;
            strlcpy(oldest_name, ent->d_name, sizeof(oldest_name));
        }
    }
    closedir(dir);

    if (count <= CACHE_MAX_FILES || oldest_name[0] == '\0') {
        return;
    }

    char path[sizeof(CACHE_DIR) + CACHE_NAME_MAX + 2];
    if (snprintf(path, sizeof(path), CACHE_DIR "/%s", oldest_name) < (int)sizeof(path)) {
        ESP_LOGI(TAG, "cache full (%u files); evicting %s", count, oldest_name);
        unlink(path);
    }
}

esp_err_t trmnl_cache_write(const char *filename, const uint8_t *data, size_t len)
{
    char path[sizeof(CACHE_DIR) + CACHE_NAME_MAX + 2];
    if (!s_available || data == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cache_path(filename, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Write to a fixed temporary name and rename into place. Losing power
     * mid-write then leaves a stale .part - which the leading dot excludes from
     * both the prune scan and any lookup - rather than a truncated file under
     * the real name that trmnl_cache_has() would happily report as a hit.
     */
    FILE *f = fopen(CACHE_TMP, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot open %s: %s", CACHE_TMP, strerror(errno));
        return ESP_FAIL;
    }

    const size_t written = fwrite(data, 1, len, f);
    const int    closed  = fclose(f);

    if (written != len || closed != 0) {
        ESP_LOGW(TAG, "short write (%u of %u bytes); not caching",
                 (unsigned)written, (unsigned)len);
        unlink(CACHE_TMP);
        return ESP_FAIL;
    }

    /* FAT rename does not replace an existing target. */
    unlink(path);
    if (rename(CACHE_TMP, path) != 0) {
        ESP_LOGW(TAG, "cannot rename into %s: %s", path, strerror(errno));
        unlink(CACHE_TMP);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "cached %s (%u bytes)", filename, (unsigned)len);
    prune();
    return ESP_OK;
}
