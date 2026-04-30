/**
 * @file
 *
 * @brief   Adapter implementation for the RapidYAML parser library.
 *
 * Include this file in your program to enable support for RapidYAML.
 *
 * This file defines the following classes (not in this order):
 *  - RymlAdapter
 *  - RymlArray
 *  - RymlArrayValueIterator
 *  - RymlFrozenValue
 *  - RymlObject
 *  - RymlObjectMember
 *  - RymlObjectMemberIterator
 *  - RymlValue
 *
 * Due to the dependencies that exist between these classes, the ordering of
 * class declarations and definitions may be a bit confusing. The best place to
 * start is RymlAdapter. This class definition is actually very small, since
 * most of the functionality is inherited from the BasicAdapter class. Most of
 * the classes in this file are provided as template arguments to the inherited
 * BasicAdapter class.
 */

#pragma once

#include <optional>
#include <string>
#include <cstdint>

#include <ryml.hpp>
#include <ryml_std.hpp>
#include <c4/charconv.hpp>

#include <valijson/internal/adapter.hpp>
#include <valijson/internal/basic_adapter.hpp>
#include <valijson/internal/frozen_value.hpp>
#include <valijson/exceptions.hpp>

namespace valijson {
namespace adapters {

class RymlAdapter;
class RymlArrayValueIterator;
class RymlObjectMemberIterator;

typedef std::pair<std::string, RymlAdapter> RymlObjectMember;

/**
 * @brief  Light weight wrapper for a RapidYAML array value.
 *
 * This class is a light weight wrapper for a RapidYAML sequence node. It
 * provides a minimum set of container functions and typedefs that allow it to
 * be used as an iterable container.
 *
 * An instance of this class contains a single ConstNodeRef to the underlying
 * RapidYAML sequence node, so there is very little overhead associated with
 * copy construction and passing by value.
 */
class RymlArray
{
  public:
    typedef RymlArrayValueIterator const_iterator;
    typedef RymlArrayValueIterator iterator;

    /// Construct a RymlArray referencing an empty array singleton.
    RymlArray() : m_value(emptyArrayRef()) {}

    /**
     * @brief   Construct a RymlArray referencing a specific RapidYAML node.
     *
     * @param   value   ConstNodeRef referencing a sequence node
     *
     * Note that this constructor will throw an exception if the value is not
     * a sequence.
     */
    RymlArray(const ryml::ConstNodeRef &value) : m_value(value)
    {
        if (!value.is_seq()) {
            throwRuntimeError("Value is not an array.");
        }
    }

    /// Return an iterator for the first element of the array.
    RymlArrayValueIterator begin() const;

    /// Return an iterator for one-past the last element of the array.
    RymlArrayValueIterator end() const;

    /// Return the number of elements in the array.
    size_t size() const
    {
        return static_cast<size_t>(m_value.num_children());
    }

  private:
    static ryml::ConstNodeRef emptyArrayRef()
    {
        static ryml::Tree s_emptyTree = ryml::parse_in_arena(ryml::to_csubstr("[]"));
        return s_emptyTree.docref(0);
    }

    /// Reference to the contained sequence node.
    ryml::ConstNodeRef m_value;
};

/**
 * @brief  Light weight wrapper for a RapidYAML object (map) value.
 *
 * This class is a light weight wrapper for a RapidYAML map node. It provides
 * a minimum set of container functions and typedefs that allow it to be used
 * as an iterable container.
 */
class RymlObject
{
  public:
    typedef RymlObjectMemberIterator const_iterator;
    typedef RymlObjectMemberIterator iterator;

    /// Construct a RymlObject referencing an empty object singleton.
    RymlObject() : m_value(emptyObjectRef()) {}

    /**
     * @brief   Construct a RymlObject referencing a specific RapidYAML node.
     *
     * @param   value  ConstNodeRef referencing a map node
     *
     * Note that this constructor will throw an exception if the value is not
     * a map.
     */
    RymlObject(const ryml::ConstNodeRef &value) : m_value(value)
    {
        if (!value.is_map()) {
            throwRuntimeError("Value is not an object.");
        }
    }

    /// Return an iterator for the first object member.
    RymlObjectMemberIterator begin() const;

    /// Return an iterator for one-past the last object member.
    RymlObjectMemberIterator end() const;

    /**
     * @brief   Return an iterator for the object member with the specified
     *          property name.
     *
     * If an object member with the specified name does not exist, the iterator
     * returned will be the same as the iterator returned by the end() function.
     *
     * @param   propertyName  property name to search for
     */
    RymlObjectMemberIterator find(const std::string &propertyName) const;

    /// Returns the number of members belonging to this object.
    size_t size() const
    {
        return static_cast<size_t>(m_value.num_children());
    }

  private:
    static ryml::ConstNodeRef emptyObjectRef()
    {
        static ryml::Tree s_emptyTree = ryml::parse_in_arena(ryml::to_csubstr("{}"));
        return s_emptyTree.docref(0);
    }

    /// Reference to the contained map node.
    ryml::ConstNodeRef m_value;
};

/**
 * @brief   Stores an independent copy of a RapidYAML value.
 *
 * This class allows a RapidYAML value to be stored independently of its
 * original document. It serializes the node back to YAML text and re-parses
 * it into a new tree so that no references to the original tree are kept.
 *
 * @see FrozenValue
 */
class RymlFrozenValue : public FrozenValue
{
  public:
    /**
     * @brief  Make an independent copy of a RapidYAML node.
     *
     * @param  source  the ConstNodeRef to be copied
     */
    explicit RymlFrozenValue(const ryml::ConstNodeRef &source)
        : m_sourceHadKey(source.has_key())
    {
        // Serialise the sub-tree to YAML text and re-parse into a private tree
        // so that the frozen value owns its data completely.
        ryml::emitrs_yaml(*(source.tree()), source.id(), &m_serialized);
        m_tree = ryml::parse_in_arena(ryml::to_csubstr(m_serialized));
    }

    FrozenValue *clone() const override
    {
        return new RymlFrozenValue(getRootNode());
    }

    bool equalTo(const Adapter &other, bool strict) const override;

  private:
    ryml::ConstNodeRef getRootNode() const
    {
        // Navigate past stream/doc wrapper nodes to the actual content node.
        ryml::ConstNodeRef root = m_tree.rootref();
        if (root.is_stream() && root.num_children() > 0) {
            root = root.first_child();
        }
        if (root.is_doc() && root.num_children() > 0) {
            root = root.first_child();
        }

        // If the original source was an object member value, freeze should
        // keep the value node, not the synthetic one-entry map container.
        if (m_sourceHadKey && root.is_map() && root.num_children() == 1) {
            root = root.first_child();
        }

        return root;
    }

    /// YAML text of the serialised node (owns the string data for m_tree).
    std::string m_serialized;
    /// The re-parsed tree that owns the frozen value.
    ryml::Tree m_tree;
    /// Tracks whether the original source node carried an object key.
    bool m_sourceHadKey;
};

/**
 * @brief   Light weight wrapper for a RapidYAML value.
 *
 * This class is passed as an argument to the BasicAdapter template class,
 * and is used to provide access to a RapidYAML value. This class is
 * responsible for the mechanics of actually reading a RapidYAML value,
 * whereas the BasicAdapter class is responsible for the semantics of type
 * comparisons and conversions.
 *
 * RapidYAML stores all scalar values as string views with no built-in type
 * information, so this adapter uses hasStrictTypes() = false, meaning that
 * all non-null scalars are returned as strings and type inference is
 * performed by the BasicAdapter's maybe*() helpers.
 *
 * @see BasicAdapter
 */
class RymlValue
{
  public:
    /// Construct a wrapper for the empty object singleton.
    RymlValue() : m_value(emptyObjectRef()) {}

    /// Construct a wrapper for a specific RapidYAML node.
    explicit RymlValue(const ryml::ConstNodeRef &value) : m_value(value) {}

    /**
     * @brief   Create a new RymlFrozenValue instance that contains the value
     *          referenced by this RymlValue instance.
     *
     * @returns pointer to a new RymlFrozenValue instance, belonging to the
     * caller.
     */
    FrozenValue *freeze() const
    {
        return new RymlFrozenValue(m_value);
    }

    /**
     * @brief   Optionally return a RymlArray instance.
     *
     * If the referenced node is a sequence, this function will return a
     * std::optional containing a RymlArray instance referencing it.
     * Otherwise it will return an empty optional.
     */
    std::optional<RymlArray> getArrayOptional() const
    {
        if (m_value.is_seq()) {
            return std::make_optional(RymlArray(m_value));
        }
        return {};
    }

    /**
     * @brief   Retrieve the number of elements in the array.
     *
     * @param   result  reference to size_t to set with result
     * @returns true if the number of elements was retrieved, false otherwise.
     */
    bool getArraySize(size_t &result) const
    {
        if (m_value.is_seq()) {
            result = static_cast<size_t>(m_value.num_children());
            return true;
        }
        return false;
    }

    bool getBool(bool &result) const
    {
        if (!m_value.has_val()) {
            return false;
        }
        const ryml::csubstr v = m_value.val();
        if (v == "true" || v == "True" || v == "TRUE") {
            result = true;
            return true;
        }
        if (v == "false" || v == "False" || v == "FALSE") {
            result = false;
            return true;
        }
        return false;
    }

    bool getDouble(double &result) const
    {
        if (!m_value.has_val()) {
            return false;
        }
        return c4::from_chars(m_value.val(), &result);
    }

    bool getInteger(int64_t &result) const
    {
        if (!m_value.has_val()) {
            return false;
        }
        return c4::from_chars(m_value.val(), &result);
    }

    /**
     * @brief   Optionally return a RymlObject instance.
     *
     * If the referenced node is a map, this function will return a
     * std::optional containing a RymlObject instance referencing it.
     * Otherwise it will return an empty optional.
     */
    std::optional<RymlObject> getObjectOptional() const
    {
        if (m_value.is_map()) {
            return std::make_optional(RymlObject(m_value));
        }
        return {};
    }

    /**
     * @brief   Retrieve the number of members in the object.
     *
     * @param   result  reference to size_t to set with result
     * @returns true if the number of members was retrieved, false otherwise.
     */
    bool getObjectSize(size_t &result) const
    {
        if (m_value.is_map()) {
            result = static_cast<size_t>(m_value.num_children());
            return true;
        }
        return false;
    }

    bool getString(std::string &result) const
    {
        if (!m_value.has_val()) {
            return false;
        }
        const ryml::csubstr v = m_value.val();
        result.assign(v.str, v.len);

        // JSON parsing in RapidYAML preserves escape sequences in scalar text.
        // Decode them here so validator string constraints operate on actual
        // Unicode characters rather than literal backslash escapes.
        if (m_value.is_val_quoted() && result.find('\\') != std::string::npos) {
            std::string decoded;
            if (decodeJsonEscapes(result, decoded)) {
                result.swap(decoded);
            }
        }

        std::string normalized;
        if (normalizeUtf8SurrogatePairs(result, normalized)) {
            result.swap(normalized);
        }

        return true;
    }

    static bool hasStrictTypes()
    {
        return true;
    }

    bool isArray() const
    {
        return m_value.is_seq();
    }

    bool isBool() const
    {
        if (!m_value.has_val() || m_value.is_val_quoted()) {
            return false;
        }

        bool result = false;
        return getBool(result);
    }

    bool isDouble() const
    {
        if (!m_value.has_val() || m_value.is_val_quoted()) {
            return false;
        }

        const ryml::csubstr v = m_value.val();
        return v.is_real() && !v.is_integer();
    }

    bool isInteger() const
    {
        if (!m_value.has_val() || m_value.is_val_quoted()) {
            return false;
        }

        return m_value.val().is_integer();
    }

    bool isNull() const
    {
        if (m_value.is_map() || m_value.is_seq()) {
            return false;
        }
        if (!m_value.has_val()) {
            return false;
        }
        return m_value.val_is_null();
    }

    bool isNumber() const
    {
        if (!m_value.has_val() || m_value.is_val_quoted()) {
            return false;
        }

        return m_value.val().is_number();
    }

    bool isObject() const
    {
        return m_value.is_map();
    }

    bool isString() const
    {
        if (!m_value.has_val()) {
            return false;
        }

        if (m_value.is_val_quoted()) {
            return true;
        }

        return !isNull() && !isBool() && !isNumber();
    }

  private:
    static int hexToInt(char c)
    {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    }

    static bool appendUtf8(uint32_t cp, std::string &out)
    {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            return false;
        }

        return true;
    }

    static bool parseHexCodePoint(const std::string &input, size_t pos,
                                  uint32_t &cp)
    {
        if (pos + 4 > input.size()) {
            return false;
        }

        cp = 0;
        for (size_t i = 0; i < 4; ++i) {
            const int value = hexToInt(input[pos + i]);
            if (value < 0) {
                return false;
            }
            cp = (cp << 4) | static_cast<uint32_t>(value);
        }

        return true;
    }

    static bool decodeJsonEscapes(const std::string &input, std::string &out)
    {
        out.clear();
        out.reserve(input.size());

        for (size_t i = 0; i < input.size(); ++i) {
            const char c = input[i];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }

            if (++i >= input.size()) {
                return false;
            }

            const char esc = input[i];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!parseHexCodePoint(input, i + 1, cp)) {
                    return false;
                }
                i += 4;

                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (i + 2 >= input.size() || input[i + 1] != '\\'
                            || input[i + 2] != 'u') {
                        return false;
                    }

                    uint32_t low = 0;
                    if (!parseHexCodePoint(input, i + 3, low)
                            || low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }

                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    i += 6;
                }

                if (!appendUtf8(cp, out)) {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
        }

        return true;
    }

    static bool decodeUtf8CodePoint(const std::string &input, size_t &i,
                                    uint32_t &cp)
    {
        if (i >= input.size()) {
            return false;
        }

        const uint8_t b0 = static_cast<uint8_t>(input[i]);
        if ((b0 & 0x80u) == 0) {
            cp = b0;
            ++i;
            return true;
        }

        auto continuation = [&input](size_t pos, uint8_t &byte) -> bool {
            if (pos >= input.size()) {
                return false;
            }
            byte = static_cast<uint8_t>(input[pos]);
            return (byte & 0xC0u) == 0x80u;
        };

        if ((b0 & 0xE0u) == 0xC0u) {
            uint8_t b1 = 0;
            if (!continuation(i + 1, b1)) {
                return false;
            }
            cp = static_cast<uint32_t>(b0 & 0x1Fu) << 6;
            cp |= static_cast<uint32_t>(b1 & 0x3Fu);
            i += 2;
            return true;
        }

        if ((b0 & 0xF0u) == 0xE0u) {
            uint8_t b1 = 0, b2 = 0;
            if (!continuation(i + 1, b1) || !continuation(i + 2, b2)) {
                return false;
            }
            cp = static_cast<uint32_t>(b0 & 0x0Fu) << 12;
            cp |= static_cast<uint32_t>(b1 & 0x3Fu) << 6;
            cp |= static_cast<uint32_t>(b2 & 0x3Fu);
            i += 3;
            return true;
        }

        if ((b0 & 0xF8u) == 0xF0u) {
            uint8_t b1 = 0, b2 = 0, b3 = 0;
            if (!continuation(i + 1, b1) || !continuation(i + 2, b2)
                    || !continuation(i + 3, b3)) {
                return false;
            }
            cp = static_cast<uint32_t>(b0 & 0x07u) << 18;
            cp |= static_cast<uint32_t>(b1 & 0x3Fu) << 12;
            cp |= static_cast<uint32_t>(b2 & 0x3Fu) << 6;
            cp |= static_cast<uint32_t>(b3 & 0x3Fu);
            i += 4;
            return true;
        }

        return false;
    }

    static bool normalizeUtf8SurrogatePairs(const std::string &input,
                                            std::string &output)
    {
        output.clear();
        output.reserve(input.size());

        size_t i = 0;
        while (i < input.size()) {
            const size_t start = i;
            uint32_t cp = 0;
            if (!decodeUtf8CodePoint(input, i, cp)) {
                return false;
            }

            if (cp >= 0xD800 && cp <= 0xDBFF) {
                const size_t lowStart = i;
                uint32_t low = 0;
                if (!decodeUtf8CodePoint(input, i, low)
                        || low < 0xDC00 || low > 0xDFFF) {
                    output.append(input, start, lowStart - start);
                    i = lowStart;
                    continue;
                }

                const uint32_t combined =
                        0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                if (!appendUtf8(combined, output)) {
                    return false;
                }
                continue;
            }

            if (!appendUtf8(cp, output)) {
                return false;
            }
        }

        return true;
    }

    static ryml::ConstNodeRef emptyObjectRef()
    {
        static ryml::Tree s_emptyTree = ryml::parse_in_arena(ryml::to_csubstr("{}"));
        return s_emptyTree.docref(0);
    }

    /// Reference to the contained RapidYAML node.
    ryml::ConstNodeRef m_value;
};

/**
 * @brief   An implementation of the Adapter interface supporting RapidYAML.
 *
 * This class is defined in terms of the BasicAdapter template class, which
 * helps to ensure that all of the Adapter implementations behave consistently.
 *
 * @see Adapter
 * @see BasicAdapter
 */
class RymlAdapter
    : public BasicAdapter<RymlAdapter, RymlArray, RymlObjectMember,
                          RymlObject, RymlValue>
{
  public:
    /// Construct a RymlAdapter that contains an empty object.
    RymlAdapter() : BasicAdapter() {}

    /// Construct a RymlAdapter containing a specific RapidYAML node.
    explicit RymlAdapter(const ryml::ConstNodeRef &value)
        : BasicAdapter(RymlValue{value})
    {
    }
};

/**
 * @brief   Class for iterating over values held in a YAML sequence.
 *
 * This class provides an array iterator that dereferences as an instance of
 * RymlAdapter representing a value stored in the array.
 *
 * @see RymlArray
 */
class RymlArrayValueIterator
{
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = RymlAdapter;
    using difference_type = std::ptrdiff_t;
    using pointer = RymlAdapter *;
    using reference = RymlAdapter &;

    /**
     * @brief   Construct a new RymlArrayValueIterator using an existing
     *          RapidYAML child iterator.
     *
     * @param   itr  RapidYAML child iterator to store
     */
    explicit RymlArrayValueIterator(
        const c4::yml::detail::child_iterator<c4::yml::ConstNodeRef> &itr)
        : m_itr(itr)
    {
    }

    /// Returns a RymlAdapter that contains the value of the current element.
    RymlAdapter operator*() const
    {
        return RymlAdapter(*m_itr);
    }

    DerefProxy<RymlAdapter> operator->() const
    {
        return DerefProxy<RymlAdapter>(**this);
    }

    bool operator==(const RymlArrayValueIterator &other) const
    {
        return m_itr == other.m_itr;
    }

    bool operator!=(const RymlArrayValueIterator &other) const
    {
        return !(m_itr == other.m_itr);
    }

    const RymlArrayValueIterator &operator++()
    {
        ++m_itr;
        return *this;
    }

    RymlArrayValueIterator operator++(int)
    {
        RymlArrayValueIterator pre(m_itr);
        ++(*this);
        return pre;
    }

    void advance(std::ptrdiff_t n)
    {
        for (std::ptrdiff_t i = 0; i < n; ++i) {
            ++m_itr;
        }
    }

  private:
    c4::yml::detail::child_iterator<c4::yml::ConstNodeRef> m_itr;
};

/**
 * @brief   Class for iterating over the members belonging to a YAML map.
 *
 * This class provides an object member iterator that dereferences as an
 * instance of RymlObjectMember representing one of the members of the object.
 *
 * @see RymlObject
 * @see RymlObjectMember
 */
class RymlObjectMemberIterator
{
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = RymlObjectMember;
    using difference_type = std::ptrdiff_t;
    using pointer = RymlObjectMember *;
    using reference = RymlObjectMember &;

    /**
     * @brief   Construct an iterator from a RapidYAML child iterator.
     *
     * @param   itr  RapidYAML child iterator to store
     */
    explicit RymlObjectMemberIterator(
        const c4::yml::detail::child_iterator<c4::yml::ConstNodeRef> &itr)
        : m_itr(itr)
    {
    }

    /**
     * @brief   Returns a RymlObjectMember containing the key and value of the
     *          current map member.
     */
    RymlObjectMember operator*() const
    {
        const ryml::ConstNodeRef node = *m_itr;
        const ryml::csubstr k = node.key();
        return RymlObjectMember(std::string(k.str, k.len), RymlAdapter(node));
    }

    DerefProxy<RymlObjectMember> operator->() const
    {
        return DerefProxy<RymlObjectMember>(**this);
    }

    bool operator==(const RymlObjectMemberIterator &other) const
    {
        return m_itr == other.m_itr;
    }

    bool operator!=(const RymlObjectMemberIterator &other) const
    {
        return !(m_itr == other.m_itr);
    }

    const RymlObjectMemberIterator &operator++()
    {
        ++m_itr;
        return *this;
    }

    RymlObjectMemberIterator operator++(int)
    {
        RymlObjectMemberIterator pre(m_itr);
        ++(*this);
        return pre;
    }

  private:
    c4::yml::detail::child_iterator<c4::yml::ConstNodeRef> m_itr;
};

/// Specialisation of the AdapterTraits template struct for RymlAdapter.
template <> struct AdapterTraits<valijson::adapters::RymlAdapter>
{
    typedef ryml::Tree DocumentType;

    static std::string adapterName()
    {
        return "RymlAdapter";
    }
};

inline bool RymlFrozenValue::equalTo(const Adapter &other, bool strict) const
{
    return RymlAdapter(getRootNode()).equalTo(other, strict);
}

inline RymlArrayValueIterator RymlArray::begin() const
{
    return RymlArrayValueIterator(m_value.begin());
}

inline RymlArrayValueIterator RymlArray::end() const
{
    return RymlArrayValueIterator(m_value.end());
}

inline RymlObjectMemberIterator RymlObject::begin() const
{
    return RymlObjectMemberIterator(m_value.begin());
}

inline RymlObjectMemberIterator RymlObject::end() const
{
    return RymlObjectMemberIterator(m_value.end());
}

inline RymlObjectMemberIterator
RymlObject::find(const std::string &propertyName) const
{
    for (auto itr = begin(); itr != end(); ++itr) {
        if ((*itr).first == propertyName) {
            return itr;
        }
    }
    return end();
}

} // namespace adapters
} // namespace valijson
