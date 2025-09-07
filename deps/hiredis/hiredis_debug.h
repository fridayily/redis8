//
// Created by L M on 2025/9/7.
//

#ifndef REDIS8_0_3_DEBUG_HIREDIS_DEBUG_H
#define REDIS8_0_3_DEBUG_HIREDIS_DEBUG_H

#define D(...) do { \
fprintf(stderr, "hiredis:%s:%d: ", __func__, __LINE__); \
fprintf(stderr, __VA_ARGS__); \
fprintf(stderr, "\n"); \
} while(0)

#endif //REDIS8_0_3_DEBUG_HIREDIS_DEBUG_H