#ifndef INCLUDE_INJA_JSON_HPP_
#define INCLUDE_INJA_JSON_HPP_

#ifdef INJA_DATA_TYPE_HEADER
#define INJA_STRINGIZE2(x) #x
#define INJA_STRINGIZE(x) INJA_STRINGIZE2(x)
#include INJA_STRINGIZE(INJA_DATA_TYPE_HEADER)
#undef INJA_STRINGIZE
#undef INJA_STRINGIZE2
#else
#include <ryml.hpp>
#endif

namespace inja {
namespace json {
#ifndef INJA_DATA_TYPE
using node = ryml::NodeRef;
using const_node = ryml::ConstNodeRef;
using pointer = ryml::Pointer;
#else
using node = INJA_DATA_TYPE;
using const_node = INJA_DATA_TYPE;
using pointer = INJA_POINTER_TYPE;
#endif
} // namespace json
} // namespace inja

#endif // INCLUDE_INJA_JSON_HPP_
