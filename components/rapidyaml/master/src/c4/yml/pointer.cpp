#include "c4/yml/pointer.hpp"
#include "c4/yml/common.hpp"
#include <string.h>

namespace c4 {
namespace yml {

Pointer::Pointer(csubstr path)
    : m_path()
{
    _parse(path);
}

void Pointer::push(csubstr fragment)
{
    RYML_ASSERT(!fragment.empty());
    m_path.push_back(fragment);
}

bool Pointer::pop()
{
    if(m_path.empty())
        return false;
    m_path.pop_back();
    return true;
}

bool Pointer::operator== (Pointer const& that) const noexcept
{
    if(m_path.size() != that.m_path.size())
        return false;
    
    for(size_t i = 0; i < m_path.size(); ++i)
    {
        if(m_path[i] != that.m_path[i])
            return false;
    }
    
    return true;
}

void Pointer::_parse(csubstr path)
{
    m_path.clear();
    
    if(path.empty())
        return;
    
    // Skip leading slash if present
    size_t pos = 0;
    if(path.len > 0 && path.str[0] == '/')
        pos = 1;
    
    // Parse each segment separated by '/'
    while(pos < path.len)
    {
        // Find next slash
        size_t next_slash = pos;
        while(next_slash < path.len && path.str[next_slash] != '/')
            ++next_slash;
        
        // Extract segment (skip empty segments)
        if(next_slash > pos)
        {
            csubstr segment(path.str + pos, next_slash - pos);
            m_path.push_back(segment);
        }
        
        // Move to next segment
        pos = next_slash + 1;
    }
}

} // namespace yml
} // namespace c4
