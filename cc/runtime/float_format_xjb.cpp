#include <stddef.h>

#if __has_include("vendor/xjb_ftoa.cpp")
#include "vendor/xjb_ftoa.cpp"
#elif __has_include("../../third_party/xjb/src/ftoa.cpp")
#include "../../third_party/xjb/src/ftoa.cpp"
#elif __has_include("../../../third_party/xjb/src/ftoa.cpp")
#include "../../../third_party/xjb/src/ftoa.cpp"
#else
#error "xjb ftoa source not found"
#endif

extern "C" char* cc_xjb_f64_to_string(double v, char *buf) {
    return xjb::xjb64(v, buf);
}

extern "C" char* cc_xjb_f32_to_string(float v, char *buf) {
    return xjb::xjb32(v, buf);
}
