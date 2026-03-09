#ifndef _C4_YML_POINTER_HPP_
#define _C4_YML_POINTER_HPP_

/** @file pointer.hpp
 * @see Pointer */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include "c4/yml/export.hpp"
#include "c4/substr.hpp"
#include "c4/yml/common.hpp"

#ifdef __clang__
#   pragma clang diagnostic push
#   pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wold-style-cast"
#elif defined(_MSC_VER)
#   pragma warning(push)
#endif

namespace c4 {
namespace yml {

/** A pointer to a location in a YAML tree. Similar to JSON Pointer (RFC 6901),
 * this class represents a path through a YAML tree as a sequence of string
 * fragments. Each fragment can be used to navigate from one node to its child.
 *
 * The pointer stores fragments as csubstr (const string views), allowing it
 * to reference existing strings without copying. This makes it efficient for
 * navigating YAML trees.
 *
 * Example usage:
 * @code{.cpp}
 * Pointer ptr;
 * ptr.push("foo");
 * ptr.push("bar");
 * // ptr now represents the path /foo/bar
 *
 * // Can also construct from a string path
 * Pointer ptr2("/foo/bar");
 * @endcode
 */
class RYML_EXPORT Pointer
{
public:

    /** @name Construction */
    /** @{ */

    /** Construct an empty pointer */
    Pointer() noexcept : m_path() {}

    /** Construct from a path string like "/foo/bar"
     * @param path A string in the format "/segment1/segment2/..."
     * The leading slash is optional. */
    explicit Pointer(csubstr path);

    /** Construct from a std::string path. The string is copied internally. */
    explicit Pointer(std::string const& path);

    /** Construct from a std::string_view path. The view is copied internally. */
    explicit Pointer(std::string_view path);

    /** Construct from a vector of path segments */
    explicit Pointer(std::vector<csubstr> const& segments);

    /** Construct from a string literal
     * @param path A string literal in the format "/segment1/segment2/..."
     * The leading slash is optional. */
    template<size_t N>
    explicit Pointer(const char (&path)[N]);

    /** @} */

public:

    /** @name Path manipulation */
    /** @{ */

    /** Add a path segment to the end
     * @param fragment The path fragment to append (should not contain '/') */
    void push(csubstr fragment);

    /** Append a path fragment and return this pointer, enabling chaining.
     *
     * Example: ptr / "foo" / "bar";
     */
    Pointer& operator/ (csubstr fragment);

    /** Append a path fragment from std::string and return this pointer,
     * enabling chaining.
     *
     * Example: ptr / std::string("foo") / std::string("bar");
     */
    Pointer& operator/ (std::string const& fragment);

    /** Append a path fragment from another Pointer and return this pointer,
     * enabling chaining.
     *
     * Example: ptr / other_ptr;
     */
    Pointer& operator/ (Pointer const& fragment);

    /** Remove the last path segment
     * @return true if a segment was removed, false if already empty */
    bool pop();

    /** Clear all path segments */
    C4_ALWAYS_INLINE void clear() noexcept { m_path.clear(); }

    /** @} */

public:

    /** @name Path queries */
    /** @{ */

    /** @return true if the pointer is empty (refers to root) */
    C4_ALWAYS_INLINE C4_PURE bool empty() const noexcept { return m_path.empty(); }

    /** @return the number of path segments */
    C4_ALWAYS_INLINE C4_PURE size_t size() const noexcept { return m_path.size(); }

    /** Get a path segment by index
     * @param i The segment index (0-based)
     * @return the segment at position i */
    C4_ALWAYS_INLINE C4_PURE csubstr operator[] (size_t i) const noexcept
    {
        RYML_ASSERT(i < m_path.size());
        return m_path[i];
    }

    /** Get the last path segment
     * @return the last segment */
    C4_ALWAYS_INLINE C4_PURE csubstr back() const noexcept
    {
        RYML_ASSERT(!m_path.empty());
        return m_path.back();
    }

    /** Get the first path segment
     * @return the first segment */
    C4_ALWAYS_INLINE C4_PURE csubstr front() const noexcept
    {
        RYML_ASSERT(!m_path.empty());
        return m_path.front();
    }

    /** Get direct access to the path segments
     * @return a const reference to the vector of path fragments */
    C4_ALWAYS_INLINE C4_PURE std::vector<csubstr> const& path() const noexcept { return m_path; }

    /** Alias for path(), matching JSON pointer naming in some integrations */
    C4_ALWAYS_INLINE C4_PURE std::vector<csubstr> const& tokens() const noexcept { return m_path; }

    /** @} */

public:

    /** @name Comparison operators */
    /** @{ */

    bool operator== (Pointer const& that) const noexcept;
    C4_ALWAYS_INLINE bool operator!= (Pointer const& that) const noexcept { return !(*this == that); }

    /** @} */

private:

    /** Parse a path string and populate m_path
     * @param path The path string to parse */
    void _parse(csubstr path);

    std::string m_storage;
    std::vector<csubstr> m_path;
};

} // namespace yml
} // namespace c4

#ifdef __clang__
#   pragma clang diagnostic pop
#elif defined(__GNUC__)
#   pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#   pragma warning(pop)
#endif

#endif // _C4_YML_POINTER_HPP_
