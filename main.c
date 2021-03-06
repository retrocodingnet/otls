#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "mbedtls/x509_crt.h"
#include "platform.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"

#include "tls_debug.h"

struct ssl_context_t {
  SOCKET sock; 
};

static inline int is_error(int errcode) {
  return errcode < 0;
}

static int tls_send_cb(void *ssl_ctx, const unsigned char *buf, size_t len) {
  struct ssl_context_t *ctx = (struct ssl_context_t *)ssl_ctx;
  
  return send(ctx->sock, buf, len, 0);
}

static int tls_recv_cb(void *ssl_ctx, unsigned char *buf, size_t len) {
  struct ssl_context_t *ctx = (struct ssl_context_t *)ssl_ctx;

  return recv(ctx->sock, (void*) buf, len, 0);
}

int main(int argc, char **argv) {
  // Winsock data 
  struct hostent *entry;
  struct in_addr **addr_list, **addr_iter, addr_item;
  struct sockaddr_in addr;
  static const char httpbin[] = "httpbin.org";
  static const short port = 443;
  static const char request [] = 
    "GET /get HTTP/1.1\r\n" /* Request Line */
    "Host: httpbin.org\r\n"
    "Connection: close\r\n"
    "User-Agent: Retrocoder/1.0\r\n"
    "Accept: application/json\r\n"
    "Content-Length: 0\r\n"
    "\r\n";
  SOCKET client = INVALID_SOCKET;
  size_t i = 0;
  struct ssl_context_t ctx;

  uint8_t buf[MTU];
  char response[BUF_MAX_SIZE]; // 4KB buffer

  int res = -1;
  
  // TLS Structures 
  mbedtls_entropy_context entropy;
  mbedtls_ssl_context ssl;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ssl_config ssl_config;
  mbedtls_x509_crt certs;

  printf("Starting Up...\n");
   
  // Result
  res = init_socket();

  if (res == SOCKET_ERROR) {
    goto error;
  }

  // Configuring TLS 
  printf("Initialising MbedTLS structures....\n");
  mbedtls_ssl_config_init(&ssl_config);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&ctr_drbg); 
  mbedtls_ssl_init(&ssl); 
  mbedtls_ssl_config_init(&ssl_config);
  mbedtls_x509_crt_init(&certs);

#if 0
  mbedtls_ssl_conf_verify(&ssl_config, my_verify, NULL);
  mbedtls_ssl_conf_dbg(&ssl_config, my_debug, NULL);
  mbedtls_debug_set_threshold(4);
#endif
 
  // Initiialising config 
  res = mbedtls_ssl_config_defaults(&ssl_config,
      MBEDTLS_SSL_IS_CLIENT, 
      MBEDTLS_SSL_TRANSPORT_STREAM,
      MBEDTLS_SSL_PRESET_DEFAULT);

  if (is_error(res)) {
    goto tls_cleanup;
  }

  mbedtls_ssl_conf_min_version(&ssl_config, 
        MBEDTLS_SSL_MAJOR_VERSION_3, 
        MBEDTLS_SSL_MINOR_VERSION_3);

  res = mbedtls_x509_crt_parse_file(&certs, CACERTS);

  if (is_error(res)) {
    goto tls_cleanup;
  }

 
  mbedtls_ssl_conf_ca_chain(&ssl_config, 
      &certs, 0);

  res = mbedtls_ssl_set_hostname(&ssl, httpbin);

  if (is_error(res)) {
    goto tls_cleanup;
  }

  mbedtls_ssl_conf_authmode(&ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);

  printf("Loaded certificates: %s\n", CACERTS);
  
  // Seeding random 
  res = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0); 

  if (is_error(res)) {
    goto tls_cleanup;
  }

  mbedtls_ssl_conf_rng(&ssl_config, mbedtls_ctr_drbg_random, &ctr_drbg);

  // host entries (this is allocated by gethostbyname) 
  entry = gethostbyname(httpbin);
  
  if ( entry == NULL ) {
    res = get_last_socket_error();
    goto error;
  }

  printf("Address resolved for %s\n", httpbin);

  addr_list = (struct in_addr **) entry->h_addr_list; // (1)
  addr_iter = addr_list; // (2) 
  
  i = 0;
  while (*addr_iter != NULL) { 
    memcpy(&addr_item, *addr_iter, entry->h_length); // (3)
    printf("%s\n", inet_ntoa(addr_item)); // (4) 
    ++addr_iter;  // (5)
    ++i;
  }

  printf("%zu addresses resolved.\n\n", i);

  memset(&addr, 0, sizeof(struct sockaddr_in));

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  memcpy(&addr.sin_addr, addr_list[0], entry->h_length);

  client = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  
  if (INVALID_SOCKET == client) {
    goto error;
  } 

  printf("Socket opened 0x%X\n", client);

  res = connect(client, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
  
  if (SOCKET_ERROR == res) {
    goto error;
  }

  printf("Connected to %s (%s)\n", inet_ntoa(addr.sin_addr), httpbin);
  
  res = mbedtls_ssl_setup(&ssl, &ssl_config);

  if (is_error(res)) {
    goto tls_cleanup;
  }

  ctx.sock = client;

  printf("Setting up BIO and doing handshake..\n");

  mbedtls_ssl_set_bio(&ssl, &ctx, tls_send_cb, tls_recv_cb, NULL);

  res = mbedtls_ssl_handshake(&ssl);

  if (is_error(res)) {
    goto tls_cleanup;
  }

  printf("Writing to TLS socket ... \n");
  
  do {
    res = mbedtls_ssl_write(&ssl, (const unsigned char *) request, sizeof(request) - 1); 

    if (res > 0) {
      break;
    }

    if (res == MBEDTLS_ERR_SSL_WANT_WRITE || res == MBEDTLS_ERR_SSL_WANT_READ) {
      continue;
    }

    if (is_error(res)) {
      printf("TLS Writing error! %d\n\n", res);
      goto error;
    }
  } while(1);

  printf("Receiving Data...\n");
  
  i = 0;
  do {
    res = mbedtls_ssl_read(&ssl, buf, MTU);

    if (res == MBEDTLS_ERR_SSL_WANT_READ || res == MBEDTLS_ERR_SSL_WANT_WRITE ) {
      continue;
    }

    if (res == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      break;
    }

    if (is_error(res)) {
      printf("Read error... -0x%X\n", res);
      goto tls_cleanup;
    }

    if (i + res > BUF_MAX_SIZE) {
      printf("Buffer overflowing..  : %lu\n", i + res);
      break;
    }

    if (res == 0) {
      printf("EOF from server \n");
      break; 
    }
  
    memcpy(response + i, buf, res);
    i += res;
    printf("Received %d bytes, total %lu\n", res, i);

  } while (1); 

  mbedtls_ssl_close_notify( &ssl );
  
  printf("Response:\n\n");
  
  fwrite(response, sizeof(char), i, stdout);

  res = closesocket(client); 

  if ( SOCKET_ERROR == res ) {
    goto error;
  }

  printf("Socket 0x%X closed\n", client);

tls_cleanup:
  // Cleaning up TLS 
  printf("Cleaning Up mbedTLS structures...\n");
  mbedtls_x509_crt_free(&certs);
  mbedtls_ssl_config_free(&ssl_config);
  mbedtls_ssl_free(&ssl);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);

  if (is_error(res)) {
    printf("Get TLS error: -%X\n", res * -1);
    goto error;
  }

  printf("Cleaning Up...\n");
  cleanup_socket();
 
  printf("Press [ENTER] to Close.\n");
  getchar();

  return res;

error:
  if (client != INVALID_SOCKET) {
    closesocket(client);
  }
  cleanup_socket();

  return res;
}
