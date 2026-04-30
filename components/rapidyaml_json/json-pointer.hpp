#pragma once

#include <string>
#include <string_view>
#include <stdexcept>
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


using ryml_pointer = json_pointer;
using yaml_pointer = json_pointer;
using toml_pointer = json_pointer;
