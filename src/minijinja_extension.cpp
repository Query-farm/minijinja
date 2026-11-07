#define DUCKDB_EXTENSION_MAIN

#include "minijinja_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "utf8proc_wrapper.hpp"

#include "rust.h"
#include "query_farm_telemetry.hpp"

#include <algorithm>

namespace duckdb {

// The options shared by every minijinja_render* function. They are keyword-only parameters, so DuckDB matches
// the names, casts the values to the declared types and fills in the defaults before the bind callback runs.
static void AddRenderOptions(ScalarFunction &function) {
	function.GetSignature()
	    .AddKeywordOnly("autoescape", LogicalType::BOOLEAN, Value::BOOLEAN(true))
	    .AddKeywordOnly("template_path", LogicalType::VARCHAR, Value(""))
	    .AddKeywordOnly("undefined_behavior", LogicalType::VARCHAR, Value("lenient"))
	    .AddKeywordOnly("autoescape_extensions", LogicalType::LIST(LogicalType::VARCHAR),
	                    Value::LIST(LogicalType::VARCHAR, vector<Value>()))
	    .AddKeywordOnly("fuel", LogicalType::UBIGINT, Value::UBIGINT(0));
}

//! A template file loaded for template_path: its name (the path relative to the template directory) and text
struct MinijinjaTemplateFile {
	string name;
	string source;
};

struct MinijinjaRenderBindData : public FunctionData {
	bool has_context;
	string template_path;
	bool autoescape = true;
	vector<string> autoescape_on;
	uint8_t undefined_behavior;
	uint64_t fuel;
	//! The files found under template_path, read once at bind time (null for inline templates)
	shared_ptr<const vector<MinijinjaTemplateFile>> template_files;

	MinijinjaRenderBindData(bool has_context_p, string template_path_p, bool autoescape_p,
	                        vector<string> autoescape_on_p, uint8_t undefined_behavior_p, uint64_t fuel_p,
	                        shared_ptr<const vector<MinijinjaTemplateFile>> template_files_p)
	    : has_context(has_context_p), template_path(std::move(template_path_p)), autoescape(autoescape_p),
	      autoescape_on(std::move(autoescape_on_p)), undefined_behavior(undefined_behavior_p), fuel(fuel_p),
	      template_files(std::move(template_files_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MinijinjaRenderBindData>(has_context, template_path, autoescape, autoescape_on,
		                                          undefined_behavior, fuel, template_files);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MinijinjaRenderBindData>();
		return has_context == other.has_context && template_path == other.template_path &&
		       autoescape == other.autoescape && autoescape_on == other.autoescape_on &&
		       undefined_behavior == other.undefined_behavior && fuel == other.fuel &&
		       template_files == other.template_files;
	}
};

//! Upper bounds on what template_path may load
static constexpr idx_t MAX_TEMPLATE_FILE_SIZE = 64ULL * 1024ULL * 1024ULL;
static constexpr idx_t MAX_TEMPLATE_TOTAL_SIZE = 256ULL * 1024ULL * 1024ULL;
static constexpr idx_t MAX_TEMPLATE_FILES = 10000;

//! The directory part of a path or glob: everything up to and including the last separator before the first
//! glob character (or before the end). Template names are the paths relative to it.
static string BaseDirectory(const string &pattern) {
	auto first_glob = pattern.find_first_of("*?[");
	auto search_end = first_glob == string::npos ? pattern.size() : first_glob;
	if (search_end == 0) {
		return string();
	}
	auto sep = pattern.find_last_of("/\\", search_end - 1);
	if (sep == string::npos) {
		return string();
	}
	return pattern.substr(0, sep + 1);
}

static string TemplateNameForPath(const string &path_p, const string &base_p) {
	// Compare with forward slashes: on Windows the glob may report either separator
	const auto path = StringUtil::Replace(path_p, "\\", "/");
	const auto base = StringUtil::Replace(base_p, "\\", "/");
	string name;
	if (!base.empty() && StringUtil::StartsWith(path, base)) {
		name = path.substr(base.size());
	} else if (StringUtil::StartsWith(base, "./") && StringUtil::StartsWith(path, base.substr(2))) {
		name = path.substr(base.size() - 2);
	} else if (base.empty() && StringUtil::StartsWith(path, "./")) {
		name = path.substr(2);
	} else if (base.empty()) {
		name = path;
	} else {
		auto sep = path.find_last_of("/\\");
		name = sep == string::npos ? path : path.substr(sep + 1);
	}
	return name;
}

//! Loads the templates for template_path through DuckDB's file system, so that enable_external_access,
//! allowed_directories and allowed_paths apply (and remote file systems such as S3 work).
//!
//! template_path is a directory (every file below it is loaded, named by its relative path), a file (the files
//! next to it are loaded) or a glob (the matching files are loaded, named relative to the glob's directory).
static shared_ptr<const vector<MinijinjaTemplateFile>> LoadTemplateFiles(ClientContext &context, const string &name,
                                                                         const string &template_path) {
	// The client file system checks enable_external_access, allowed_directories and allowed_paths, throwing a
	// PermissionException when the path is not allowed.
	auto &fs = FileSystem::GetFileSystem(context);
	const bool is_directory = StringUtil::EndsWith(template_path, "/") || StringUtil::EndsWith(template_path, "\\");
	string pattern;
	if (FileSystem::HasGlob(template_path)) {
		pattern = template_path;
	} else if (!is_directory && fs.FileExists(template_path)) {
		pattern = BaseDirectory(template_path) + "*";
	} else {
		pattern = is_directory ? template_path + "**" : template_path + "/**";
	}
	const auto base = BaseDirectory(pattern);

	auto files = make_shared_ptr<vector<MinijinjaTemplateFile>>();
	idx_t total_size = 0;
	for (auto &match : fs.GlobFiles(pattern, FileGlobOptions::ALLOW_EMPTY)) {
		if (fs.DirectoryExists(match.path)) {
			continue;
		}
		if (files->size() >= MAX_TEMPLATE_FILES) {
			throw InvalidInputException("%s: template_path '%s' matches more than %llu files", name, template_path,
			                            MAX_TEMPLATE_FILES);
		}
		auto handle = fs.OpenFile(match, FileFlags::FILE_FLAGS_READ);
		const auto size = handle->GetFileSize();
		if (size > MAX_TEMPLATE_FILE_SIZE) {
			throw InvalidInputException("%s: template file '%s' is larger than %llu bytes", name, match.path,
			                            MAX_TEMPLATE_FILE_SIZE);
		}
		total_size += size;
		if (total_size > MAX_TEMPLATE_TOTAL_SIZE) {
			throw InvalidInputException("%s: the files under template_path '%s' total more than %llu bytes", name,
			                            template_path, MAX_TEMPLATE_TOTAL_SIZE);
		}
		string source;
		source.resize(size);
		idx_t offset = 0;
		while (offset < size) {
			auto read = handle->Read(&source[offset], size - offset);
			if (read <= 0) {
				break;
			}
			offset += NumericCast<idx_t>(read);
		}
		source.resize(offset);
		if (!Utf8Proc::IsValid(source.c_str(), source.size())) {
			throw InvalidInputException("%s: template file '%s' is not valid UTF-8", name, match.path);
		}
		files->push_back(MinijinjaTemplateFile {TemplateNameForPath(match.path, base), std::move(source)});
	}
	return files;
}

static uint8_t ParseUndefinedBehavior(const string &name, const string &value) {
	if (value == "lenient") {
		return UNDEFINED_LENIENT;
	}
	if (value == "strict") {
		return UNDEFINED_STRICT;
	}
	if (value == "semi_strict") {
		return UNDEFINED_SEMI_STRICT;
	}
	if (value == "chainable") {
		return UNDEFINED_CHAINABLE;
	}
	throw BinderException("%s: 'undefined_behavior' argument has invalid value '%s' must be one of: lenient, "
	                      "strict, chainable, semi_strict",
	                      name, value);
}

static unique_ptr<FunctionData> MinijinjaRenderBindInternal(BindScalarFunctionInput &input, bool has_context) {
	const auto &name = input.GetBoundFunction().GetName().GetIdentifierName();

	const auto autoescape = BooleanValue::Get(input.GetNonNullConstant("autoescape"));
	auto template_path = StringValue::Get(input.GetNonNullConstant("template_path"));
	const auto undefined_behavior =
	    ParseUndefinedBehavior(name, StringValue::Get(input.GetNonNullConstant("undefined_behavior")));
	const auto fuel = UBigIntValue::Get(input.GetNonNullConstant("fuel"));

	// Keep the list Value alive for the loop: ListValue::GetChildren returns a reference into it, and a temporary in
	// a range-for initializer is destroyed before the loop body runs.
	const auto autoescape_extensions = input.GetNonNullConstant("autoescape_extensions");
	vector<string> autoescape_on;
	for (const auto &extension : ListValue::GetChildren(autoescape_extensions)) {
		if (extension.IsNull()) {
			throw BinderException("%s: 'autoescape_extensions' must not contain NULL", name);
		}
		autoescape_on.push_back(StringValue::Get(extension));
	}

	shared_ptr<const vector<MinijinjaTemplateFile>> template_files;
	if (!template_path.empty()) {
		template_files = LoadTemplateFiles(input.GetClientContext(), name, template_path);
	}

	return make_uniq<MinijinjaRenderBindData>(has_context, std::move(template_path), autoescape,
	                                          std::move(autoescape_on), undefined_behavior, fuel,
	                                          std::move(template_files));
}

static unique_ptr<FunctionData> MinijinjaRenderBind(BindScalarFunctionInput &input) {
	return MinijinjaRenderBindInternal(input, false);
}

static unique_ptr<FunctionData> MinijinjaRenderWithContextBind(BindScalarFunctionInput &input) {
	return MinijinjaRenderBindInternal(input, true);
}

struct CompiledEnvironmentDeleter {
	void operator()(CompiledEnvironment *compiled) const {
		free_compiled_environment(compiled);
	}
};
using CompiledEnvironmentPtr = std::unique_ptr<CompiledEnvironment, CompiledEnvironmentDeleter>;

// Parsing a template costs far more than rendering it, so each thread keeps the templates it has parsed for the
// duration of the query and renders every row from them.
struct MinijinjaRenderLocalState : public FunctionLocalState {
	//! Inline templates, keyed by template text. A constant template argument is parsed once; a template that
	//! differs between rows is cached up to MAX_INLINE_TEMPLATES distinct texts at a time.
	static constexpr idx_t MAX_INLINE_TEMPLATES = 64;
	unordered_map<string, CompiledEnvironmentPtr> inline_templates;
	//! The most recently used inline template, checked before the map
	string last_template;
	optional_ptr<CompiledEnvironment> last_compiled;
	//! The environment serving the template_path templates (which MiniJinja parses on first use); created on
	//! the first non-NULL row
	CompiledEnvironmentPtr path_templates;
};

static unique_ptr<FunctionLocalState>
MinijinjaRenderInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	return make_uniq<MinijinjaRenderLocalState>();
}

static CompiledEnvironmentPtr CompileEnvironment(const MinijinjaRenderBindData &bind_data, const char *source,
                                                 idx_t source_len) {
	vector<const char *> autoescape_on_ptrs;
	for (const auto &s : bind_data.autoescape_on) {
		autoescape_on_ptrs.push_back(s.c_str());
	}
	vector<TemplateSource> templates;
	if (bind_data.template_files) {
		for (const auto &file : *bind_data.template_files) {
			templates.push_back(
			    TemplateSource {file.name.c_str(), file.name.size(), file.source.c_str(), file.source.size()});
		}
	}
	EnvironmentOptions options {bind_data.autoescape, bind_data.undefined_behavior, autoescape_on_ptrs.data(),
	                            autoescape_on_ptrs.size(), bind_data.fuel};
	CompileResult compiled = compile_environment(source, source_len, bind_data.template_files != nullptr,
	                                             templates.data(), templates.size(), &options);
	if (compiled.tag == CompileResult::Tag::Err) {
		string err_str = string(compiled.err._0);
		free_compile_error(compiled.err._0);
		throw InvalidInputException("Error rendering template: " + err_str);
	}
	return CompiledEnvironmentPtr(compiled.ok._0);
}

static CompiledEnvironment &GetCompiledEnvironment(MinijinjaRenderLocalState &lstate,
                                                   const MinijinjaRenderBindData &bind_data, string_t expression) {
	if (bind_data.template_files) {
		// The template argument names a template within the loaded set
		if (!lstate.path_templates) {
			lstate.path_templates = CompileEnvironment(bind_data, "", 0);
		}
		return *lstate.path_templates;
	}

	if (lstate.last_compiled && expression.GetSize() == lstate.last_template.size() &&
	    memcmp(expression.GetData(), lstate.last_template.data(), expression.GetSize()) == 0) {
		return *lstate.last_compiled;
	}
	auto key = expression.GetString();
	auto entry = lstate.inline_templates.find(key);
	if (entry == lstate.inline_templates.end()) {
		auto compiled = CompileEnvironment(bind_data, expression.GetData(), expression.GetSize());
		if (lstate.inline_templates.size() >= MinijinjaRenderLocalState::MAX_INLINE_TEMPLATES) {
			lstate.last_compiled = nullptr;
			lstate.inline_templates.clear();
		}
		entry = lstate.inline_templates.emplace(key, std::move(compiled)).first;
	}
	lstate.last_template = std::move(key);
	lstate.last_compiled = entry->second.get();
	return *lstate.last_compiled;
}

static string_t RenderToVector(Vector &result, MinijinjaRenderLocalState &lstate,
                               const MinijinjaRenderBindData &bind_data, string_t expression, const char *context_json,
                               idx_t context_json_len) {
	auto &compiled = GetCompiledEnvironment(lstate, bind_data, expression);
	ResultCString eval_result =
	    render_compiled(&compiled, expression.GetData(), expression.GetSize(), context_json, context_json_len);
	if (eval_result.tag == ResultCString::Tag::Err) {
		string err_str = string(eval_result.err._0);
		free_result_cstring(eval_result);
		throw InvalidInputException("Error rendering template: " + err_str);
	}
	auto vector_result = StringVector::AddString(result, eval_result.ok._0);
	free_result_cstring(eval_result);
	return vector_result;
}

inline void MinijinjaRenderFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &bind_data = func_expr.BindInfo()->Cast<MinijinjaRenderBindData>();
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<MinijinjaRenderLocalState>();

	// The keyword-only options follow the positional arguments in the chunk; their values were captured at bind.
	auto &expression_vector = args.data[0];

	if (bind_data.has_context) {
		auto &context_json_vector = args.data[1];
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    expression_vector, context_json_vector, result, [&](string_t expression, string_t context_json) {
			    return RenderToVector(result, lstate, bind_data, expression, context_json.GetData(),
			                          context_json.GetSize());
		    });
	} else {
		UnaryExecutor::Execute<string_t, string_t>(expression_vector, result, [&](string_t expression) {
			return RenderToVector(result, lstate, bind_data, expression, "{}", 2);
		});
	}
}

static FunctionDescription MakeDescription(vector<string> names, vector<LogicalType> types, string description,
                                           vector<string> examples) {
	FunctionDescription desc;
	desc.description = std::move(description);
	// The keyword-only options, in the order AddRenderOptions declares them
	names.insert(names.end(), {"autoescape", "template_path", "undefined_behavior", "autoescape_extensions", "fuel"});
	types.insert(types.end(), {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                           LogicalType::LIST(LogicalType::VARCHAR), LogicalType::UBIGINT});
	desc.parameter_names = std::move(names);
	desc.parameter_types = std::move(types);
	desc.examples = std::move(examples);
	desc.categories = {"text", "template"};
	return desc;
}

static constexpr const char *OPTIONS_HELP =
    " Options are passed by name: autoescape (default true), template_path (a directory, file or glob of template "
    "files read through DuckDB's file system, default none; the template argument is then a template name), "
    "undefined_behavior (lenient, strict, chainable or semi_strict; default lenient), autoescape_extensions "
    "(default []) and fuel (the maximum number of instructions one render may execute, default 0 = unlimited).";

static void RegisterRenderFunction(ExtensionLoader &loader, const char *function_name, bool has_context,
                                   const string &description, vector<string> examples) {
	ScalarFunctionSet set(function_name);
	ScalarFunction function({}, LogicalType::VARCHAR, MinijinjaRenderFunc,
	                        has_context ? MinijinjaRenderWithContextBind : MinijinjaRenderBind);
	function.GetSignature().AddParameter("template", LogicalType::VARCHAR);
	if (has_context) {
		function.GetSignature().AddParameter("context", LogicalType::JSON());
	}
	AddRenderOptions(function);
	function.SetNullHandling(FunctionNullHandling::DEFAULT_NULL_HANDLING);
	// MiniJinja rendering is deterministic (no clock or random functions are registered), so the function is not
	// volatile: constant arguments are folded and repeated expressions are evaluated once.
	function.SetFallible();
	function.SetInitStateCallback(MinijinjaRenderInitLocalState);
	set.AddFunction(function);

	CreateScalarFunctionInfo info(set);
	vector<string> names {"template"};
	vector<LogicalType> types {LogicalType::VARCHAR};
	if (has_context) {
		names.push_back("context");
		types.push_back(LogicalType::JSON());
	}
	info.descriptions.push_back(
	    MakeDescription(std::move(names), std::move(types), description + OPTIONS_HELP, std::move(examples)));
	loader.RegisterFunction(info);
}

// Extension initalization.
static void LoadInternal(ExtensionLoader &loader) {
	// minijinja_render(template, *, autoescape, template_path, undefined_behavior, autoescape_extensions, fuel)
	RegisterRenderFunction(loader, "minijinja_render", false,
	                       "Render a Jinja2-style template using the MiniJinja templating engine.",
	                       {"minijinja_render('Hello World!')",
	                        "minijinja_render('Hello & Goodbye!', autoescape := false)",
	                        "minijinja_render('{% for i in range(3) %}{{ i }}{% endfor %}')"});

	// minijinja_render_with_context(template, context, *, autoescape, template_path, undefined_behavior,
	//                               autoescape_extensions, fuel)
	RegisterRenderFunction(
	    loader, "minijinja_render_with_context", true,
	    "Render a Jinja2-style template with a JSON object as its context.",
	    {"minijinja_render_with_context('Hello {{ name }}!', '{\"name\": \"World\"}')",
	     "minijinja_render_with_context('{{ items | join(\", \") }}', '{\"items\": [1, 2, 3]}')",
	     "minijinja_render_with_context('{{ name | title }}', '{\"name\": \"ada lovelace\"}', undefined_behavior := "
	     "'strict')"});

	QueryFarmSendTelemetry(loader, "minijinja", MINIJINJA_EXTENSION_VERSION);
}

void MinijinjaExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string MinijinjaExtension::Name() {
	return "minijinja";
}

std::string MinijinjaExtension::Version() const {
	return MINIJINJA_EXTENSION_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(minijinja, loader) {
	duckdb::LoadInternal(loader);
}
}
