#ifndef INCLUDE_INJA_JSON_HPP_
#define INCLUDE_INJA_JSON_HPP_

#ifdef INJA_DATA_TYPE
#ifdef INJA_DATA_TYPE_HEADER
#define INJA_STRINGIZE2(x) #x
#define INJA_STRINGIZE(x) INJA_STRINGIZE2(x)
#include INJA_STRINGIZE(INJA_DATA_TYPE_HEADER)
#undef INJA_STRINGIZE
#undef INJA_STRINGIZE2
#endif
#else
#include <nlohmann/json.hpp>
#endif

namespace inja {
#ifndef INJA_DATA_TYPE
using json = nlohmann::json;
#else
using json = INJA_DATA_TYPE;
#endif
} // namespace inja

#endif // INCLUDE_INJA_JSON_HPP_
