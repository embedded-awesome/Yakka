#pragma once

#include "ryml.hpp"
#include <expected>
#include <vector>
#include <filesystem>

class json_pointer
{
public:
	json_pointer() = default;
	explicit json_pointer(const std::string &path) { parse(path); }

	json_pointer &operator/=(const std::string &segment)
	{
		segments_.push_back(segment);
		return *this;
	}

	json_pointer &operator/=(size_t index)
	{
		segments_.push_back(std::to_string(index));
		return *this;
	}

	friend json_pointer operator/(json_pointer lhs, const std::string &segment)
	{
		lhs /= segment;
		return lhs;
	}

	friend json_pointer operator/(json_pointer lhs, size_t index)
	{
		lhs /= index;
		return lhs;
	}

	std::string to_string() const
	{
		std::string out;
		for (const auto &seg : segments_) {
			out += "/" + seg;
		}
		return out;
	}

	const std::vector<std::string> &parts() const { return segments_; }

	bool empty() const noexcept { return segments_.empty(); }

	const std::string &back() const
	{
		if (segments_.empty())
			throw std::out_of_range("json_pointer has no parent");
		return segments_.back();
	}

	void pop_back()
	{
		if (segments_.empty())
			throw std::out_of_range("json_pointer has no parent");
		segments_.pop_back();
	}

private:
	void parse(const std::string &path)
	{
		segments_.clear();
		if (path.empty())
			return;
		std::string_view view(path);
		size_t start = (view.front() == '/') ? 1 : 0;
		while (start <= view.size()) {
			size_t slash = view.find('/', start);
			if (slash == std::string_view::npos)
				slash = view.size();
			if (slash > start)
				segments_.emplace_back(view.substr(start, slash - start));
			if (slash >= view.size())
				break;
			start = slash + 1;
		}
	}

	std::vector<std::string> segments_;
};

// struct ryml_pointer {
//   ryml_pointer() = default;
//   explicit ryml_pointer(std::string path);
//   explicit ryml_pointer(std::vector<std::string> segments_in);

//   const std::vector<std::string> &parts() const noexcept;
//   bool empty() const noexcept;

// private:
//   std::vector<std::string> segments;
// };

using ryml_pointer = json_pointer;
using yaml_pointer = json_pointer;
using toml_pointer = json_pointer;

// ryml_pointer operator/(ryml_pointer lhs, const ryml_pointer &rhs);
// ryml_pointer &operator/=(ryml_pointer &lhs, const ryml_pointer &rhs);
// ryml_pointer operator/(ryml_pointer lhs, const std::string &segment);
// ryml_pointer &operator/=(ryml_pointer &lhs, const std::string &segment);
// ryml_pointer operator/(ryml_pointer lhs, size_t index);
// ryml_pointer &operator/=(ryml_pointer &lhs, size_t index);


// RapidYAML conversion utilities
// ryml::Tree ryml_to_json(const ryml::ConstNodeRef &node);
// inja::json ryml_to_inja_json(const ryml::ConstNodeRef &node);

std::string ryml_val_string(const ryml::ConstNodeRef &node);
bool ryml_has_child(const ryml::ConstNodeRef &node, c4::csubstr key);
ryml::ConstNodeRef ryml_get_child(const ryml::ConstNodeRef &node, c4::csubstr key);
bool ryml_has_path(const ryml::ConstNodeRef &node, const ryml_pointer &path);
ryml::ConstNodeRef ryml_get_path(const ryml::ConstNodeRef &node, const ryml_pointer &path);
ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const std::vector<std::string> &path, bool create_if_missing = true);
ryml::NodeRef ryml_navigate_path(ryml::NodeRef node, const ryml_pointer &path, bool create_if_missing = true);
std::expected<ryml::Tree, std::error_code> ryml_load_file(const std::filesystem::path &path);

inline ryml::NodeRef ryml_create_path(ryml::NodeRef node, const ryml_pointer &path)
{
	return ryml_navigate_path(node, path, true);
}