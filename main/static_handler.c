#include "static_handler.h"
#include "littlefs_mgr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "STATIC";

/* ── MIME type table – project-wide SSOT ───────────────────────────────── */

typedef struct { const char *ext; const char *mime; } mime_entry_t;

static const mime_entry_t s_mime_map[] = {
    { ".html",  "text/html; charset=utf-8"       },
    { ".css",   "text/css; charset=utf-8"         },
    { ".js",    "application/javascript"          },
    { ".json",  "application/json"                },
    { ".svg",   "image/svg+xml"                   },
    { ".png",   "image/png"                       },
    { ".jpg",   "image/jpeg"                      },
    { ".ico",   "image/x-icon"                    },
    { ".woff2", "font/woff2"                      },
    { ".woff",  "font/woff"                       },
    { ".ttf",   "font/ttf"                        },
    { ".txt",   "text/plain; charset=utf-8"       },
    { ".xml",   "application/xml"                 },
    { ".pdf",   "application/pdf"                 },
    { NULL,     "application/octet-stream"        },  /* default / sentinel */
};

/* ── Embedded fallback (web_ui.html via EMBED_TXTFILES) ─────────────────── */

extern const uint8_t web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const uint8_t web_ui_html_end[]   asm("_binary_web_ui_html_end");

/* ── Internal helpers ───────────────────────────────────────────────────── */

/* Return a pointer to the file extension including the dot, or NULL. */
static const char *file_ext(const char *name)
{
    if (!name) return NULL;
    const char *dot = strrchr(name, '.');
    /* Ignore a leading dot (hidden files like ".htaccess"). */
    return (dot && dot != name) ? dot : NULL;
}

/* Build a weak ETag from the file size.
 * NOTE: LittleFS does not populate st_mtime (always 0), so the ETag is
 * based on file size only.  A content-changing same-size update will NOT
 * invalidate the cache.  Mitigate by versioning filenames (e.g. app.v2.js)
 * or by clearing browser cache after a LittleFS partition update. */
static void make_etag(const struct stat *st, char *buf, size_t len)
{
    snprintf(buf, len, "W/\"%lx\"", (unsigned long)st->st_size);
}

/* Send a single file from the VFS.  Checks for a .gz sibling first. */
static esp_err_t send_file(httpd_req_t *req, const char *vfs_path,
                            const char *mime)
{
    /* Try pre-compressed variant first. */
    char gz_path[256];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", vfs_path);

    struct stat st;
    bool        use_gz = (stat(gz_path, &st) == 0);
    const char *path   = use_gz ? gz_path : vfs_path;

    if (!use_gz && stat(vfs_path, &st) != 0) {
        ESP_LOGW(TAG, "File not found: %s", vfs_path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open failed");
        return ESP_FAIL;
    }

    /* Headers. */
    httpd_resp_set_type(req, mime);
    if (use_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    char etag[40];
    make_etag(&st, etag, sizeof(etag));
    httpd_resp_set_hdr(req, "ETag", etag);

    /* Cache strategy: HTML always revalidated; static assets cached 24 h.
     * IMPORTANT: use vfs_path (not path) for extension detection because
     * path may point to the .gz variant – file_ext(".gz") would wrongly
     * return "no-cache" for all pre-compressed assets. */
    const char *ext = file_ext(vfs_path);
    bool is_static = ext && (
        strcasecmp(ext, ".js")    == 0 ||
        strcasecmp(ext, ".css")   == 0 ||
        strcasecmp(ext, ".ico")   == 0 ||
        strcasecmp(ext, ".png")   == 0 ||
        strcasecmp(ext, ".jpg")   == 0 ||
        strcasecmp(ext, ".woff2") == 0 ||
        strcasecmp(ext, ".woff")  == 0 ||
        strcasecmp(ext, ".ttf")   == 0
    );
    httpd_resp_set_hdr(req, "Cache-Control",
                       is_static ? "public, max-age=86400" : "no-cache");

    /* Stream file in PSRAM-backed chunks. */
    const size_t CHUNK = 4096;
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(CHUNK);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    free(buf);
    fclose(f);

    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0); /* End chunked response. */
    }
    return err;
}

/* ── URI handlers ───────────────────────────────────────────────────────── */

/* GET /favicon.ico – return 204 No Content if no file exists in LittleFS.
 * Prevents the browser from generating a 404 log entry on every page load. */
static esp_err_t favicon_handler(httpd_req_t *req)
{
    if (littlefs_mgr_is_mounted()) {
        char path[128];
        snprintf(path, sizeof(path), "%s/favicon.ico", littlefs_mgr_base_path());
        struct stat st;
        if (stat(path, &st) == 0) {
            return send_file(req, path, "image/x-icon");
        }
    }
    /* No favicon available – respond with 204 to suppress browser retries. */
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* GET /  – tries LittleFS index.html, falls back to embedded web_ui.html. */
static esp_err_t root_handler(httpd_req_t *req)
{
    if (littlefs_mgr_is_mounted()) {
        char path[128];
        snprintf(path, sizeof(path), "%s/index.html",
                 littlefs_mgr_base_path());
        struct stat st;
        if (stat(path, &st) == 0) {
            return send_file(req, path, "text/html; charset=utf-8");
        }
        ESP_LOGW(TAG, "LittleFS mounted but index.html missing – using fallback.");
    }

    /* Embedded fallback. */
    ESP_LOGI(TAG, "Serving embedded fallback web_ui.html.");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req,
                    (const char *)web_ui_html_start,
                    (ssize_t)(web_ui_html_end - web_ui_html_start));
    return ESP_OK;
}

/* Serves any static asset from LittleFS (wildcard). */
static esp_err_t static_file_handler(httpd_req_t *req)
{
    if (!littlefs_mgr_is_mounted()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"Filesystem not available\"}");
        return ESP_FAIL;
    }

    /* Build the VFS path: /www<uri>  (uri already starts with '/'). */
    /* URI can be up to 512 bytes; base_path is short. 520 is sufficient. */
    char path[520];
    snprintf(path, sizeof(path), "%s%s",
             littlefs_mgr_base_path(), req->uri);

    /* Strip query string if present. */
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    const char *mime = static_handler_mime(path);
    return send_file(req, path, mime);
}

/* httpd requires a wildcard URI for catch-all; registered with is_uri_match_wildcard. */
static const httpd_uri_t s_favicon_uri = {
    .uri     = "/favicon.ico",
    .method  = HTTP_GET,
    .handler = favicon_handler,
};

static const httpd_uri_t s_root_uri = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = root_handler,
};

static const httpd_uri_t s_static_uri = {
    .uri     = "/*",
    .method  = HTTP_GET,
    .handler = static_file_handler,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

const char *static_handler_mime(const char *filename)
{
    if (!filename) return s_mime_map[0].mime; /* octet-stream sentinel */
    const char *ext = file_ext(filename);
    if (!ext) goto fallback;

    for (const mime_entry_t *m = s_mime_map; m->ext != NULL; m++) {
        if (strcasecmp(ext, m->ext) == 0) return m->mime;
    }
fallback:
    /* Last entry in the table is the catch-all. */
    return s_mime_map[sizeof(s_mime_map) / sizeof(s_mime_map[0]) - 1].mime;
}

esp_err_t static_handler_register(httpd_handle_t server)
{
    esp_err_t err;
    err = httpd_register_uri_handler(server, &s_favicon_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register '/favicon.ico': %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &s_root_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register '/': %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &s_static_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register '/*': %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
