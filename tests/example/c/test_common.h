#include <cmocka.h>


#define UNUSED(...) unused( (void *) NULL, __VA_ARGS__);
static inline void unused(void *dummy, ...) { (void)(dummy);}