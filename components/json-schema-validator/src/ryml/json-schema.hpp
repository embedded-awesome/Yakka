#ifndef NLOHMANN_JSON_SCHEMA_HPP__
#define NLOHMANN_JSON_SCHEMA_HPP__

#ifdef _WIN32
#	if defined(JSON_SCHEMA_VALIDATOR_EXPORTS)
#		define JSON_SCHEMA_VALIDATOR_API __declspec(dllexport)
#	elif defined(JSON_SCHEMA_VALIDATOR_IMPORTS)
#		define JSON_SCHEMA_VALIDATOR_API __declspec(dllimport)
#	else
#		define JSON_SCHEMA_VALIDATOR_API
#	endif
#else
#	define JSON_SCHEMA_VALIDATOR_API
#endif

#include "ryml.hpp"
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ryml_schema
{
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

	friend json_pointer operator/(json_pointer lhs, const std::string &segment)
	{
		lhs /= segment;
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

class json_uri
{
public:
	explicit json_uri(std::string uri = "#") : uri_(std::move(uri)) {}
	std::string to_string() const { return uri_; }
	std::string location() const
	{
		auto pos = uri_.find('#');
		if (pos == std::string::npos)
			return uri_;
		return uri_.substr(0, pos);
	}
	std::string fragment() const
	{
		auto pos = uri_.find('#');
		if (pos == std::string::npos)
			return "";
		return uri_.substr(pos + 1);
	}
	json_pointer pointer() const
	{
		auto frag = fragment();
		if (frag.empty())
			return json_pointer{""};
		if (!frag.empty() && frag.front() != '/')
			return json_pointer{"/" + frag};
		return json_pointer{frag};
	}
	json_uri append(const std::string &field) const
	{
		if (uri_.empty())
			return json_uri{"#/" + field};
		if (uri_.back() == '#')
			return json_uri{uri_ + "/" + field};
		auto hash = uri_.find('#');
		if (hash == std::string::npos)
			return json_uri{uri_ + "#/" + field};
		return json_uri{uri_ + "/" + field};
	}

private:
	std::string uri_;
};

// using json = nlohmann::json;
using json = ryml::Tree;
using schema_loader = std::function<void(const json_uri &, json &)>;
using format_checker = std::function<void(const std::string &, const std::string &, const json &, class error_handler &)>;
using content_checker = std::function<void(const std::string &, const json &, const json &, class error_handler &)>;

class JSON_SCHEMA_VALIDATOR_API error_handler
{
public:
	virtual ~error_handler() = default;
	virtual void error(const json_pointer &, const json &, const std::string &) = 0;
};

class JSON_SCHEMA_VALIDATOR_API basic_error_handler : public error_handler
{
public:
	void error(const json_pointer &, const json &, const std::string &) override {}
};

void JSON_SCHEMA_VALIDATOR_API default_string_format_check(const std::string &format,
	                                                     const std::string &value,
	                                                     const json &instance,
	                                                     error_handler &handler);

class JSON_SCHEMA_VALIDATOR_API json_validator
{
public:
	json_validator(schema_loader loader = nullptr, format_checker format = nullptr, content_checker content = nullptr);
	json_validator(const json &schema, schema_loader loader = nullptr, format_checker format = nullptr, content_checker content = nullptr);
	json_validator(json &&schema, schema_loader loader = nullptr, format_checker format = nullptr, content_checker content = nullptr);

	json_validator(json_validator &&);
	json_validator &operator=(json_validator &&);

	json_validator(const json_validator &) = delete;
	json_validator &operator=(const json_validator &) = delete;

	~json_validator();

	void set_root_schema(const json &schema);
	void set_root_schema(json &&schema);

	json validate(const json &instance) const;
	json validate(const json &instance, error_handler &err, const json_uri &initial_uri = json_uri("#")) const;

private:
	json root_schema_;
	schema_loader loader_{};
	format_checker format_checker_{};
	content_checker content_checker_{};
};

} // namespace ryml_schema

#endif /* NLOHMANN_JSON_SCHEMA_HPP__ */
