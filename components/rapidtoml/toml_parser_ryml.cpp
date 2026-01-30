/*
 * TOML Parser Reimplementation using ryml
 *
 * This file reimplements TOML parsing functionality using ryml (RapidYAML)
 * classes such as ryml::Tree and ryml::NodeRef instead of toml++ classes.
 *
 * Original toml++ parser: tomlplusplus/include/toml++/impl/parser.inl
 * Adapted to use ryml classes for tree/node representation.
 */

#include "ryml.hpp"
#include "c4/std/string.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <memory>

namespace toml_ryml
{

  using namespace std::string_view_literals;

  //-----------------------------------------------------------------------------
  // Source position tracking
  //-----------------------------------------------------------------------------

  struct source_position
  {
    uint32_t line;
    uint32_t column;

    source_position() : line(1), column(1) {}
    source_position(uint32_t l, uint32_t c) : line(l), column(c) {}
  };

  struct source_region
  {
    source_position begin;
    source_position end;
    std::string path;
  };

  //-----------------------------------------------------------------------------
  // UTF-8 codepoint representation
  //-----------------------------------------------------------------------------

  struct utf8_codepoint
  {
    char32_t value;
    char bytes[4];
    uint8_t count;
    source_position position;
  };

  //-----------------------------------------------------------------------------
  // Parse error handling
  //-----------------------------------------------------------------------------

  class parse_error : public std::runtime_error
  {
  public:
    source_position position;
    std::string source_path;

    parse_error(const std::string &msg, source_position pos, const std::string &path = "")
        : std::runtime_error(msg), position(pos), source_path(path)
    {
    }
  };

  //-----------------------------------------------------------------------------
  // UTF-8 reader interface (simplified)
  //-----------------------------------------------------------------------------

  class utf8_reader
  {
  private:
    std::string_view source_;
    size_t position_;
    source_position current_pos_;
    std::string source_path_;
    std::vector<utf8_codepoint> buffer_;
    size_t buffer_pos_;

  public:
    utf8_reader(std::string_view source, const std::string &path = "")
        : source_(source), position_(0), current_pos_(1, 1), source_path_(path), buffer_pos_(0)
    {
      // Skip UTF-8 BOM if present
      if (source_.length() >= 3 &&
          source_[0] == '\xEF' && source_[1] == '\xBB' && source_[2] == '\xBF')
      {
        position_ = 3;
      }
    }

    bool eof() const { return position_ >= source_.length(); }

    const utf8_codepoint *read_next()
    {
      if (eof())
        return nullptr;

      // Simplified UTF-8 decoding (ASCII only for now)
      if (buffer_.size() <= buffer_pos_)
        buffer_.resize(buffer_pos_ + 1);

      auto &cp = buffer_[buffer_pos_];
      cp.value = static_cast<char32_t>(source_[position_]);
      cp.bytes[0] = source_[position_];
      cp.count = 1;
      cp.position = current_pos_;

      position_++;

      if (cp.value == '\n')
      {
        current_pos_.line++;
        current_pos_.column = 1;
      }
      else
      {
        current_pos_.column++;
      }

      buffer_pos_++;
      return &cp;
    }

    const utf8_codepoint *step_back(size_t count = 1)
    {
      if (buffer_pos_ >= count)
      {
        buffer_pos_ -= count;
        return buffer_pos_ < buffer_.size() ? &buffer_[buffer_pos_] : nullptr;
      }
      return nullptr;
    }

    bool peek_eof() const { return eof(); }

    const std::string &source_path() const { return source_path_; }
  };

  //-----------------------------------------------------------------------------
  // Key buffer for dotted keys
  //-----------------------------------------------------------------------------

  struct parse_key_buffer
  {
    std::string buffer;
    std::vector<std::pair<size_t, size_t>> segments;
    std::vector<source_position> starts;
    std::vector<source_position> ends;

    void clear()
    {
      buffer.clear();
      segments.clear();
      starts.clear();
      ends.clear();
    }

    void push_back(std::string_view segment, source_position b, source_position e)
    {
      segments.push_back({buffer.length(), segment.length()});
      buffer.append(segment);
      starts.push_back(b);
      ends.push_back(e);
    }

    std::string_view operator[](size_t i) const
    {
      return std::string_view{buffer.c_str() + segments[i].first, segments[i].second};
    }

    std::string_view back() const
    {
      return (*this)[segments.size() - 1];
    }

    bool empty() const { return segments.empty(); }
    size_t size() const { return segments.size(); }
  };

  //-----------------------------------------------------------------------------
  // Helper functions for character classification
  //-----------------------------------------------------------------------------

  inline bool is_whitespace(char32_t c)
  {
    return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r';
  }

  inline bool is_horizontal_whitespace(char32_t c)
  {
    return c == U' ' || c == U'\t';
  }

  inline bool is_ascii_horizontal_whitespace(char32_t c)
  {
    return c == U' ' || c == U'\t';
  }

  inline bool is_ascii_vertical_whitespace(char32_t c)
  {
    return c == U'\n' || c == U'\r';
  }

  inline bool is_nontab_control_character(char32_t c)
  {
    return (c >= U'\x00' && c <= U'\x08') || (c >= U'\x0A' && c <= U'\x1F') || c == U'\x7F';
  }

  inline bool is_unicode_surrogate(char32_t c)
  {
    return c >= 0xD800 && c <= 0xDFFF;
  }

  inline bool is_bare_key_character(char32_t c)
  {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9') || c == U'_' || c == U'-';
  }

  inline bool is_string_delimiter(char32_t c)
  {
    return c == U'"' || c == U'\'';
  }

  inline bool is_value_terminator(char32_t c)
  {
    return c == U'#' || is_ascii_vertical_whitespace(c);
  }

  inline bool is_digit(char32_t c, int base = 10)
  {
    if (base <= 10)
      return c >= U'0' && c < (U'0' + base);
    return (c >= U'0' && c <= U'9') || (c >= U'a' && c < (U'a' + base - 10)) || (c >= U'A' && c < (U'A' + base - 10));
  }

  //-----------------------------------------------------------------------------
  // Main TOML parser using ryml::Tree
  //-----------------------------------------------------------------------------

  class toml_parser
  {
  private:
    // Reader and tree
    utf8_reader reader;
    ryml::Tree tree;

    // Parser state
    source_position prev_pos;
    const utf8_codepoint *cp;
    ryml::NodeRef root_node;
    ryml::NodeRef current_table_node;

    // Tracking structures
    std::vector<ryml::NodeRef> implicit_tables;
    std::vector<ryml::NodeRef> dotted_key_tables;
    std::vector<ryml::NodeRef> open_inline_tables;
    std::vector<ryml::NodeRef> table_arrays;

    // Buffers
    parse_key_buffer key_buffer;
    std::string string_buffer;
    std::string recording_buffer;
    bool recording;
    bool recording_whitespace;

    // Scope and depth tracking
    std::string_view current_scope;
    [[maybe_unused]] size_t nested_values;

    // Error handling
    std::optional<parse_error> err;

    static constexpr size_t max_nested_values = 200;
    static constexpr size_t max_dotted_keys_depth = 200;

    //-------------------------------------------------------------------------
    // Error handling helpers
    //-------------------------------------------------------------------------

    bool is_error() const { return err.has_value(); }
    bool is_eof() const { return cp == nullptr; }

    source_position current_position(uint32_t fallback_offset = 0) const
    {
      if (!is_eof())
        return cp->position;
      return {prev_pos.line, prev_pos.column + fallback_offset};
    }

    template <typename... Args>
    void set_error_at(source_position pos, Args &&...args)
    {
      if (is_error())
        return;

      std::string msg = "Error while parsing ";
      msg += current_scope;
      msg += ": ";
      (msg.append(args), ...);

      err.emplace(msg, pos, reader.source_path());
    }

    template <typename... Args>
    void set_error(Args &&...args)
    {
      set_error_at(current_position(1), std::forward<Args>(args)...);
    }

    //-------------------------------------------------------------------------
    // Reader navigation
    //-------------------------------------------------------------------------

    void advance()
    {
      if (is_error())
        return;

      if (is_eof())
        return;

      prev_pos = cp->position;
      cp = reader.read_next();

      if (recording && !is_eof())
      {
        if (recording_whitespace || !is_whitespace(cp->value))
          recording_buffer.append(cp->bytes, cp->count);
      }
    }

    void go_back(size_t count = 1)
    {
      if (is_error())
        return;

      cp = reader.step_back(count);
      if (cp)
        prev_pos = cp->position;
    }

    //-------------------------------------------------------------------------
    // Recording helpers
    //-------------------------------------------------------------------------

    void start_recording(bool include_current = true)
    {
      if (is_error())
        return;

      recording = true;
      recording_whitespace = true;
      recording_buffer.clear();

      if (include_current && !is_eof())
        recording_buffer.append(cp->bytes, cp->count);
    }

    void stop_recording(size_t pop_bytes = 0)
    {
      if (is_error())
        return;

      recording = false;
      if (pop_bytes)
      {
        if (pop_bytes >= recording_buffer.length())
          recording_buffer.clear();
        else if (pop_bytes == 1)
          recording_buffer.pop_back();
        else
          recording_buffer.erase(recording_buffer.end() - pop_bytes, recording_buffer.end());
      }
    }

    //-------------------------------------------------------------------------
    // Whitespace and comment consumption
    //-------------------------------------------------------------------------

    bool consume_leading_whitespace()
    {
      if (is_error() || is_eof())
        return false;

      bool consumed = false;
      while (!is_eof() && is_horizontal_whitespace(cp->value))
      {
        if (!is_ascii_horizontal_whitespace(cp->value))
        {
          set_error("expected space or tab, saw invalid whitespace");
          return false;
        }

        consumed = true;
        advance();
        if (is_error())
          return false;
      }
      return consumed;
    }

    bool consume_line_break()
    {
      if (is_error() || is_eof())
        return false;

      if (cp->value == U'\v' || cp->value == U'\f')
      {
        set_error("vertical tabs and form-feeds are not legal line breaks in TOML");
        return false;
      }

      if (cp->value == U'\r')
      {
        advance();
        if (is_error())
          return false;

        if (is_eof())
        {
          set_error("expected '\\n' after '\\r', saw EOF");
          return false;
        }

        if (cp->value != U'\n')
        {
          set_error("expected '\\n' after '\\r'");
          return false;
        }
      }
      else if (cp->value != U'\n')
      {
        return false;
      }

      advance();
      return true;
    }

    bool consume_comment()
    {
      if (is_error() || is_eof())
        return false;

      if (cp->value != U'#')
        return false;

      advance(); // skip the '#'
      if (is_error())
        return false;

      while (!is_eof())
      {
        if (consume_line_break())
          return true;
        if (is_error())
          return false;

        // Check for invalid control characters in comments
        if (is_nontab_control_character(cp->value))
        {
          set_error("control characters other than TAB are prohibited in comments");
          return false;
        }

        if (is_unicode_surrogate(cp->value))
        {
          set_error("unicode surrogates are prohibited in comments");
          return false;
        }

        advance();
        if (is_error())
          return false;
      }

      return true;
    }

    //-------------------------------------------------------------------------
    // Key parsing
    //-------------------------------------------------------------------------

    void parse_key()
    {
      if (is_error())
        return;

      if (is_eof())
      {
        set_error("expected key, saw EOF");
        return;
      }

      // Bare key
      if (is_bare_key_character(cp->value))
      {
        while (!is_eof() && is_bare_key_character(cp->value))
        {
          advance();
          if (is_error())
            return;
        }
      }
      // Quoted key (basic or literal string)
      else if (is_string_delimiter(cp->value))
      {
        // Simplified: just consume until matching quote
        char32_t delimiter = cp->value;
        advance(); // skip opening quote
        if (is_error())
          return;

        while (!is_eof() && cp->value != delimiter)
        {
          if (cp->value == U'\\' && delimiter == U'"')
          {
            advance(); // skip backslash
            if (is_error())
              return;
          }
          advance();
          if (is_error())
            return;
        }

        if (is_eof())
        {
          set_error("unexpected EOF in quoted key");
          return;
        }

        advance(); // skip closing quote
      }
      else
      {
        set_error("expected bare or quoted key");
        return;
      }

      // At this point, cp is on the character after the key (not part of the key)
      // Remove it from the recording buffer
      if (recording && !is_eof() && recording_buffer.size() >= cp->count)
      {
        recording_buffer.erase(recording_buffer.end() - cp->count, recording_buffer.end());
      }

      // Handle dotted keys
      // Temporarily disable recording while consuming whitespace
      bool was_recording = recording;
      recording = false;
      consume_leading_whitespace();
      if (!is_error() && !is_eof() && cp->value == U'.')
      {
        // Re-enable recording and add the dot
        recording = was_recording;
        if (recording)
          recording_buffer.append(cp->bytes, cp->count);
        
        advance(); // skip '.'
        if (is_error())
          return;

        recording = false;
        consume_leading_whitespace();
        recording = was_recording;
        if (is_error())
          return;

        parse_key(); // recursive call for next segment
      }
      else
      {
        // Restore recording state
        recording = was_recording;
      }
    }

    //-------------------------------------------------------------------------
    // Value parsing (stubs for now)
    //-------------------------------------------------------------------------

    std::string parse_value()
    {
      if (is_error())
        return "";

      if (is_eof())
      {
        set_error("expected value, saw EOF");
        return "";
      }

      // Determine value type and parse accordingly
      // For now, just create a simple scalar node

      // String
      if (is_string_delimiter(cp->value))
      {
        return parse_string();
      }
      // Boolean
      else if (cp->value == U't' || cp->value == U'f')
      {
        return parse_boolean();
      }
      // Array
      else if (cp->value == U'[')
      {
        return parse_array();
      }
      // Inline table
      else if (cp->value == U'{')
      {
        return parse_inline_table();
      }
      // Number (integer, float, date-time)
      else if (is_digit(cp->value) || cp->value == U'+' || cp->value == U'-')
      {
        return parse_number();
      }
      else
      {
        set_error("unexpected character in value");
        return "";
      }
    }

    std::string parse_string()
    {
      // Stub implementation
      if (is_error())
        return "";

      char32_t delimiter = cp->value;
      advance(); // skip opening quote

      std::string value;
      while (!is_eof() && cp->value != delimiter)
      {
        if (cp->value == U'\\' && delimiter == U'"')
        {
          advance(); // skip backslash
          if (!is_eof())
          {
            // Handle escape sequences (simplified)
            advance();
          }
        }
        else
        {
          value.append(cp->bytes, cp->count);
          advance();
        }
      }

      if (!is_eof())
        advance(); // skip closing quote

      return value;
    }

    std::string parse_boolean()
    {
      // Parse "true" or "false"
      std::string bool_str;
      while (!is_eof() && (cp->value == U't' || cp->value == U'r' || cp->value == U'u' || cp->value == U'e' ||
                           cp->value == U'f' || cp->value == U'a' || cp->value == U'l' || cp->value == U's'))
      {
        bool_str.append(cp->bytes, cp->count);
        advance();
        if (is_error())
          return "";
      }

      return bool_str;
    }

    void parse_array_into_node(ryml::NodeRef node)
    {
      if (!node.valid())
        return;
        
      advance(); // skip '['
      if (is_error())
        return;

      // Parse array elements
      while (!is_eof())
      {
        // Skip whitespace and comments
        consume_leading_whitespace();
        if (is_error())
          return;
          
        // Check for end of array
        if (cp->value == U']')
        {
          advance(); // skip ']'
          return;
        }
        
        // Parse array element
        if (cp->value == U'[')
        {
          // Nested array
          auto child = node.append_child();
          child |= ryml::SEQ;
          parse_array_into_node(child);
        }
        else if (cp->value == U'{')
        {
          // Inline table in array
          auto child = node.append_child();
          child |= ryml::MAP;
          parse_inline_table_into_node(child);
        }
        else
        {
          // Scalar value
          auto value_str = parse_value();
          if (is_error())
            return;
          auto child = node.append_child();
          child << value_str;
        }
        
        if (is_error())
          return;
        
        // Skip whitespace after element
        consume_leading_whitespace();
        if (is_error())
          return;
        
        // Check for comma or end of array
        if (cp->value == U',')
        {
          advance(); // skip comma
          if (is_error())
            return;
        }
        else if (cp->value != U']')
        {
          set_error("expected ',' or ']' in array");
          return;
        }
      }
      
      set_error("unexpected EOF in array");
    }
    
    std::string parse_array()
    {
      // Fallback for when called directly (shouldn't happen normally)
      std::string array_content = "[";
      advance(); // skip '['

      while (!is_eof() && cp->value != U']')
      {
        array_content.append(cp->bytes, cp->count);
        advance();
        if (is_error())
          return "[]";
      }
      
      if (!is_eof())
      {
        array_content += "]";
        advance(); // skip ']'
      }

      return array_content;
    }

    void parse_inline_table_into_node(ryml::NodeRef node)
    {
      if (!node.valid())
        return;
        
      advance(); // skip '{'
      if (is_error())
        return;

      // Parse key-value pairs
      while (!is_eof())
      {
        // Skip whitespace
        consume_leading_whitespace();
        if (is_error())
          return;
          
        // Check for end of inline table
        if (cp->value == U'}')
        {
          advance(); // skip '}'
          return;
        }
        
        // Parse key
        key_buffer.clear();
        start_recording();
        parse_key();
        stop_recording();
        
        if (is_error())
          return;
        
        // Skip whitespace before '='
        consume_leading_whitespace();
        if (is_error() || is_eof())
          return;
        
        // Expect '='
        if (cp->value != U'=')
        {
          set_error("expected '=' in inline table");
          return;
        }
        
        advance(); // skip '='
        if (is_error())
          return;
        
        // Skip whitespace after '='
        consume_leading_whitespace();
        if (is_error() || is_eof())
          return;
        
        // Parse value
        auto kv = node.append_child();
        kv << ryml::key(recording_buffer);
        
        if (cp->value == U'[')
        {
          kv |= ryml::SEQ;
          parse_array_into_node(kv);
        }
        else if (cp->value == U'{')
        {
          kv |= ryml::MAP;
          parse_inline_table_into_node(kv);
        }
        else
        {
          auto value_str = parse_value();
          if (is_error())
            return;
          kv << value_str;
        }
        
        if (is_error())
          return;
        
        // Skip whitespace after value
        consume_leading_whitespace();
        if (is_error())
          return;
        
        // Check for comma or end of inline table
        if (cp->value == U',')
        {
          advance(); // skip comma
          if (is_error())
            return;
        }
        else if (cp->value != U'}')
        {
          set_error("expected ',' or '}' in inline table");
          return;
        }
      }
      
      set_error("unexpected EOF in inline table");
    }
    
    std::string parse_inline_table()
    {
      // Fallback for when called directly (shouldn't happen normally)
      advance(); // skip '{'

      while (!is_eof() && cp->value != U'}')
        advance();
      if (!is_eof())
        advance(); // skip '}'

      return "{}";
    }

    std::string parse_number()
    {
      // Stub - simplified number/date-time parsing
      std::string num_str;
      while (!is_eof() && (is_digit(cp->value) || cp->value == U'.' ||
                           cp->value == U'+' || cp->value == U'-' ||
                           cp->value == U'e' || cp->value == U'E' ||
                           cp->value == U':' || cp->value == U'T' || cp->value == U't' ||
                           cp->value == U'Z' || cp->value == U'z'))
      {
        num_str.append(cp->bytes, cp->count);
        advance();
      }

      return num_str;
    }

    //-------------------------------------------------------------------------
    // Table header parsing
    //-------------------------------------------------------------------------

    ryml::NodeRef parse_table_header()
    {
      if (is_error())
        return ryml::NodeRef();

      if (is_eof() || cp->value != U'[')
      {
        set_error("expected '[' for table header");
        return ryml::NodeRef();
      }

      advance(); // skip first '['
      if (is_error())
        return ryml::NodeRef();

      // Check for table array [[...]]
      bool is_array = false;
      if (!is_eof() && cp->value == U'[')
      {
        is_array = true;
        advance(); // skip second '['
        if (is_error())
          return ryml::NodeRef();
      }

      consume_leading_whitespace();
      if (is_error())
        return ryml::NodeRef();

      // Parse table key
      key_buffer.clear();
      start_recording();
      parse_key();
      stop_recording();

      if (is_error())
        return ryml::NodeRef();

      consume_leading_whitespace();

      // Expect closing bracket(s)
      if (is_eof() || cp->value != U']')
      {
        set_error("expected ']' to close table header");
        return ryml::NodeRef();
      }

      advance(); // skip first ']'

      if (is_array)
      {
        if (is_eof() || cp->value != U']')
        {
          set_error("expected second ']' for table array");
          return ryml::NodeRef();
        }
        advance(); // skip second ']'
      }

      // Create or find the table node in the tree
      // Handle dotted table names by creating nested structure
      std::string table_path = recording_buffer;
      ryml::NodeRef table_node = tree.rootref();
      
      // Split the table path by dots and create nested tables
      size_t start = 0;
      size_t dot_pos = table_path.find('.');
      
      while (dot_pos != std::string::npos)
      {
        std::string segment = table_path.substr(start, dot_pos - start);
        
        // Look for existing child with this key
        ryml::NodeRef child;
        for (auto c : table_node.children())
        {
          if (c.has_key() && c.key() == ryml::csubstr(segment.c_str(), segment.length()))
          {
            child = c;
            break;
          }
        }
        
        // Create child if not found
        if (!child.valid())
        {
          child = table_node.append_child();
          child << ryml::key(segment);
          child |= ryml::MAP;
        }
        
        table_node = child;
        start = dot_pos + 1;
        dot_pos = table_path.find('.', start);
      }
      
      // Handle the last segment
      std::string last_segment = table_path.substr(start);
      
      // Look for existing child with this key
      ryml::NodeRef final_node;
      for (auto c : table_node.children())
      {
        if (c.has_key() && c.key() == ryml::csubstr(last_segment.c_str(), last_segment.length()))
        {
          final_node = c;
          break;
        }
      }
      
      // Create final node if not found
      if (!final_node.valid())
      {
        final_node = table_node.append_child();
        final_node << ryml::key(last_segment);
        final_node |= ryml::MAP;
      }

      return final_node;
    }

    //-------------------------------------------------------------------------
    // Key-value pair parsing
    //-------------------------------------------------------------------------

    bool parse_key_value_pair_and_insert(ryml::NodeRef &target_table)
    {
      if (is_error())
        return false;

      // Parse key
      key_buffer.clear();
      start_recording();
      parse_key();
      stop_recording();

      if (is_error())
        return false;

      // Skip whitespace
      consume_leading_whitespace();
      if (is_error() || is_eof())
        return false;

      // Expect '='
      if (cp->value != U'=')
      {
        set_error("expected '=', saw something else");
        return false;
      }

      advance(); // skip '='
      if (is_error())
        return false;

      // Skip whitespace after '='
      consume_leading_whitespace();
      if (is_error() || is_eof())
        return false;

      // Check for value terminator
      if (is_value_terminator(cp->value))
      {
        set_error("expected value");
        return false;
      }

      // Parse the value
      // Check if it's an array or inline table (needs special handling)
      if (target_table.valid())
      {
        auto kv = target_table.append_child();
        kv << ryml::key(recording_buffer);
        
        if (cp->value == U'[')
        {
          // Array - create sequence node and parse recursively
          kv |= ryml::SEQ;
          parse_array_into_node(kv);
        }
        else if (cp->value == U'{')
        {
          // Inline table - create map node and parse recursively
          kv |= ryml::MAP;
          parse_inline_table_into_node(kv);
        }
        else
        {
          // Scalar value
          auto value_str = parse_value();
          if (is_error())
            return false;
          kv << value_str;
        }
      }
      else
      {
        // No valid target table, still consume the value to advance parser
        parse_value();
      }

      if (is_error())
        return false;

      return true;
    }

    //-------------------------------------------------------------------------
    // Main document parsing
    //-------------------------------------------------------------------------

  public:
    void parse_document()
    {
      if (is_error())
        return;

      if (is_eof())
        return;

      current_scope = "root table"sv;

      // Initialize root as a MAP node
      root_node = tree.rootref();
      root_node |= ryml::MAP;
      current_table_node = root_node;

      do
      {
        if (is_error())
          return;

        // Skip leading whitespace, line breaks, and comments
        if (consume_leading_whitespace() || consume_line_break() || consume_comment())
          continue;

        if (is_error())
          return;

        // Parse table headers [table] or [[table array]]
        if (!is_eof() && cp->value == U'[')
        {
          current_table_node = parse_table_header();
        }
        // Parse key-value pairs
        else if (!is_eof() && (is_bare_key_character(cp->value) || is_string_delimiter(cp->value)))
        {
          current_scope = "key-value pair"sv;

          parse_key_value_pair_and_insert(current_table_node);

          // Handle rest of line after kvp
          consume_leading_whitespace();
          if (is_error())
            return;

          if (!is_eof() && !consume_comment() && !consume_line_break())
          {
            set_error("expected a comment or whitespace after key-value pair");
          }
        }
        else if (!is_eof())
        {
          set_error("expected keys, tables, whitespace or comments");
        }
      } while (!is_eof());
    }

    //-------------------------------------------------------------------------
    // Constructor and public interface
    //-------------------------------------------------------------------------

    toml_parser(std::string_view source, const std::string &source_path = "")
        : reader(source, source_path), prev_pos(1, 1), cp(nullptr), recording(false), recording_whitespace(true), nested_values(0)
    {
      // Initialize tree
      tree.clear();
      tree.rootref() |= ryml::MAP;

      // Start reading
      if (!reader.eof())
      {
        cp = reader.read_next();

        if (cp)
          parse_document();
      }
    }

    const ryml::Tree &get_tree() const { return tree; }
    ryml::Tree &get_tree() { return tree; }

    bool has_error() const { return err.has_value(); }
    const parse_error &get_error() const { return *err; }
  };

  //-----------------------------------------------------------------------------
  // Public API
  //-----------------------------------------------------------------------------

  ryml::Tree parse_toml(std::string_view source, const std::string &source_path = "")
  {
    toml_parser parser(source, source_path);

    if (parser.has_error())
    {
      throw parser.get_error();
    }

    return parser.get_tree();
  }

} // namespace toml_ryml

//-----------------------------------------------------------------------------
// Example usage
//-----------------------------------------------------------------------------

#ifdef TOML_RYML_EXAMPLE
#include <iostream>
#include <fstream>
#include <sstream>

int main()
{
  try
  {
    // Read "example.toml" into a string
    std::ifstream file("example.toml");
    if (!file.is_open())
    {
      std::cerr << "Error: Could not open example.toml\n";
      return 1;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string toml_source = buffer.str();

    auto tree = toml_ryml::parse_toml(toml_source, "example.toml");

    std::cout << "Successfully parsed TOML document into ryml::Tree\n";
    std::cout << "Tree has " << tree.size() << " nodes\n";

    // Emit the tree as YAML (since ryml is a YAML library)
    std::string output = ryml::emitrs_yaml<std::string>(tree);
    std::cout << "\nTree structure as YAML:\n"
              << output << "\n";
  }
  catch (const toml_ryml::parse_error &e)
  {
    std::cerr << "Parse error at line " << e.position.line
              << ", column " << e.position.column << ": "
              << e.what() << "\n";
    return 1;
  }

  return 0;
}
#endif
