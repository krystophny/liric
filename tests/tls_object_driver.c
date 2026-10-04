#include "tls_thread_oracle.h"

extern void *tls_counter(void);
extern void *tls_alias(void);
extern void *tls_zero(void);
extern void *tls_bytes(void);
extern void *tls_offset(void);

int main(void) {
    get_counter = tls_counter;
    get_alias = tls_alias;
    get_zero = tls_zero;
    get_bytes = tls_bytes;
    get_offset = tls_offset;
    return check_tls_storage();
}
