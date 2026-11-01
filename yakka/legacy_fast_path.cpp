#include "legacy_fast_path.hpp"
#include "glob/glob.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace yakka {
namespace {

constexpr std::array<char, 4> magic = { 'Y', 'K', 'L', 'F' };
constexpr uint32_t format_version   = 1;

class writer {
public:
  template <typename T> void value(T number)
  {
    const auto *bytes = reinterpret_cast<const char *>(&number);
    data.append(bytes, sizeof(T));
  }
  void text(const std::string &value_text)
  {
    value(uint32_t(value_text.size()));
    data += value_text;
  }
  void texts(const std::vector<std::string> &values)
  {
    value(uint32_t(values.size()));
    for (const auto &item: values)
      text(item);
  }
  std::string data;
};

class reader {
public:
  explicit reader(const std::string &content) : data(content)
  {
  }
  template <typename T> bool value(T &number)
  {
    if (data.size() - position < sizeof(T))
      return false;
    std::memcpy(&number, data.data() + position, sizeof(T));
    position += sizeof(T);
    return true;
  }
  bool text(std::string &value_text)
  {
    uint32_t size = 0;
    if (!value(size) || data.size() - position < size)
      return false;
    value_text.assign(data.data() + position, size);
    position += size;
    return true;
  }
  bool texts(std::vector<std::string> &values)
  {
    uint32_t count = 0;
    if (!value(count) || count > data.size() - position)
      return false;
    values.resize(count);
    for (auto &item: values)
      if (!text(item))
        return false;
    return true;
  }
  bool finished() const
  {
    return position == data.size();
  }

private:
  const std::string &data;
  size_t position = 0;
};

std::vector<std::string> expand(const std::vector<std::string> &patterns)
{
  std::vector<std::string> matches;
  for (const auto &path: glob::rglob(patterns))
    matches.push_back(path.generic_string());
  std::sort(matches.begin(), matches.end());
  return matches;
}

} // namespace

bool is_legacy_fast_record(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  std::array<char, 4> header{};
  return input.read(header.data(), header.size()) && header == magic;
}

bool write_legacy_fast_record(const std::filesystem::path &path, const legacy_fast_record &record)
{
  writer output;
  output.data.append(magic.data(), magic.size());
  output.value(format_version);
  output.texts(record.projects);
  output.value(uint32_t(record.files.size()));
  for (const auto &file: record.files) {
    output.text(file.path);
    output.value(uint8_t(file.stat.has_value()));
    if (file.stat) {
      output.value(file.stat->mtime);
      output.value(file.stat->ctime);
      output.value(file.stat->size);
      output.value(file.stat->inode);
    }
  }
  output.value(uint32_t(record.globs.size()));
  for (const auto &expansion: record.globs) {
    output.texts(expansion.patterns);
    output.texts(expansion.matches);
  }

  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  return file.write(output.data.data(), std::streamsize(output.data.size())) && file.flush();
}

std::optional<legacy_fast_record> read_legacy_fast_record(const std::filesystem::path &path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return std::nullopt;
  const std::string content{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
  if (content.size() < magic.size() || !std::equal(magic.begin(), magic.end(), content.begin()))
    return std::nullopt;
  reader input(content);
  std::array<char, 4> header{};
  uint32_t version = 0;
  if (!input.value(header) || !input.value(version) || version != format_version)
    return std::nullopt;

  legacy_fast_record record;
  uint32_t count = 0;
  if (!input.texts(record.projects) || !input.value(count) || count > content.size())
    return std::nullopt;
  record.files.resize(count);
  for (auto &entry: record.files) {
    uint8_t exists = 0;
    if (!input.text(entry.path) || !input.value(exists))
      return std::nullopt;
    if (exists) {
      file_stat stat;
      if (!input.value(stat.mtime) || !input.value(stat.ctime) || !input.value(stat.size) || !input.value(stat.inode))
        return std::nullopt;
      entry.stat = stat;
    }
  }
  if (!input.value(count) || count > content.size())
    return std::nullopt;
  record.globs.resize(count);
  for (auto &expansion: record.globs)
    if (!input.texts(expansion.patterns) || !input.texts(expansion.matches))
      return std::nullopt;
  if (!input.finished() || record.projects.empty())
    return std::nullopt;
  return record;
}

bool legacy_fast_record_current(const legacy_fast_record &record)
{
  try {
    for (const auto &file: record.files)
      if (stat_file(file.path) != file.stat)
        return false;
    for (const auto &expansion: record.globs)
      if (expand(expansion.patterns) != expansion.matches)
        return false;
  } catch (const std::exception &) {
    return false;
  }
  return true;
}

} // namespace yakka
