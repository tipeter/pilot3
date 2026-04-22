#include "cert_mgr.h"
#include "esp_log.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/oid.h"
#include <string.h>

static const char *TAG = "CERT_MGR";

/* SSOT: The ONLY place in the codebase where linker symbols are declared. */
extern const uint8_t server_cert_pem_start[] asm("_binary_server_crt_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_crt_end");
extern const uint8_t server_key_pem_start[]  asm("_binary_server_key_start");
extern const uint8_t server_key_pem_end[]    asm("_binary_server_key_end");

const uint8_t* cert_mgr_get_cert_pem(size_t *out_len)
{
    if (out_len) {
        /* +1 to include the null byte for mbedtls */
        *out_len = (size_t)(server_cert_pem_end - server_cert_pem_start) + 1;
    }
    return server_cert_pem_start;
}

const uint8_t* cert_mgr_get_key_pem(size_t *out_len)
{
    if (out_len) {
        *out_len = (size_t)(server_key_pem_end - server_key_pem_start) + 1;
    }
    return server_key_pem_start;
}

esp_err_t cert_mgr_get_hostname(char *out_hostname, size_t max_len)
{
  if (!out_hostname || max_len == 0)
    return ESP_ERR_INVALID_ARG;

  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);

  /* 1. Calculate raw length strictly without assuming null-termination */
  size_t raw_len = (size_t) (server_cert_pem_end - server_cert_pem_start);

  /* 2. Allocate a temporary buffer to guarantee null-termination */
  unsigned char *tmp_buf = malloc(raw_len + 1);
  if (!tmp_buf)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for cert parsing");
    return ESP_ERR_NO_MEM;
  }

  /* 3. Copy data and forcefully append the null byte */
  memcpy(tmp_buf, server_cert_pem_start, raw_len);
  tmp_buf[raw_len] = '\0';

  /* 4. mbedtls PEM parsing strictly requires length to include the null byte */
  int ret = mbedtls_x509_crt_parse(&crt, tmp_buf, raw_len + 1);
  free(tmp_buf); /* Immediately free heap buffer after parsing */

  if (ret != 0)
  {
    ESP_LOGE(TAG, "mbedtls_x509_crt_parse failed: -0x%04X", -ret);
    mbedtls_x509_crt_free(&crt);
    return ESP_FAIL;
  }

  esp_err_t err = ESP_ERR_NOT_FOUND;
  mbedtls_x509_name *name = &crt.subject;

  /* 5. Traverse the ASN.1 structure to find the Common Name (CN) */
  while (name != NULL)
  {
    if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &name->oid) == 0)
    {
      size_t len = name->val.len;
      if (len < max_len)
      {
        memcpy(out_hostname, name->val.p, len);
        out_hostname[len] = '\0';

        /* Strip ".local" suffix if it exists */
        char *dot = strstr(out_hostname, ".local");
        if (dot != NULL)
        {
          *dot = '\0';
        }
        err = ESP_OK;
      }
      else
      {
        err = ESP_ERR_INVALID_SIZE;
      }
      break;
    }
    name = name->next;
  }

  mbedtls_x509_crt_free(&crt);
  return err;
}
