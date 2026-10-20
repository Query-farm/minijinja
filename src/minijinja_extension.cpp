#define DUCKDB_EXTENSION_MAIN

#include "minijinja_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/vector/string_vector.hpp"

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
	                    Value::LIST(LogicalType::VARCHAR, vector<Value>()));
}

struct MinijinjaRenderBindData : public FunctionData {
	bool has_context;
	string template_path;
	bool autoescape = true;
	vector<string> autoescape_on;
	string undefined_behavior;

	MinijinjaRenderBindData(bool has_context_p, string template_path_p, bool autoescape_p,
	                        vector<string> autoescape_on_p, string undefined_behavior_p)
	    : has_context(has_context_p), template_path(std::move(template_path_p)), autoescape(autoescape_p),
	      autoescape_on(std::move(autoescape_on_p)), undefined_behavior(std::move(undefined_behavior_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MinijinjaRenderBindData>(has_context, template_path, autoescape, autoescape_on,
		                                          undefined_behavior);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MinijinjaRenderBindData>();
		return has_context == other.has_context && template_path == other.template_path &&
		       autoescape == other.autoescape && autoescape_on == other.autoescape_on &&
		       undefined_behavior == other.undefined_behavior;
	}
};

static unique_ptr<FunctionData> MinijinjaRenderBindInternal(BindScalarFunctionInput &input, bool has_context) {
	const auto &name = input.GetBoundFunction().GetName().GetIdentifierName();

	const auto autoescape = BooleanValue::Get(input.GetNonNullConstant("autoescape"));
	auto template_path = StringValue::Get(input.GetNonNullConstant("template_path"));

	auto undefined_behavior = StringValue::Get(input.GetNonNullConstant("undefined_behavior"));
	static const vector<string> valid_undefined_behaviors = {"lenient", "strict", "chainable", "semi_strict"};
	if (std::find(valid_undefined_behaviors.begin(), valid_undefined_behaviors.end(), undefined_behavior) ==
	    valid_undefined_behaviors.end()) {
		throw BinderException("%s: 'undefined_behavior' argument has invalid value '%s' must be one of: lenient, "
		                      "strict, chainable, semi_strict",
		                      name, undefined_behavior);
	}

	vector<string> autoescape_on;
	for (const auto &extension : ListValue::GetChildren(input.GetNonNullConstant("autoescape_extensions"))) {
		if (extension.IsNull()) {
			throw BinderException("%s: 'autoescape_extensions' must not contain NULL", name);
		}
		autoescape_on.push_back(StringValue::Get(extension));
	}

	return make_uniq<MinijinjaRenderBindData>(has_context, std::move(template_path), autoescape,
	                                          std::move(autoescape_on), std::move(undefined_behavior));
}

static unique_ptr<FunctionData> MinijinjaRenderBind(BindScalarFunctionInput &input) {
	return MinijinjaRenderBindInternal(input, false);
}

static unique_ptr<FunctionData> MinijinjaRenderWithContextBind(BindScalarFunctionInput &input) {
	return MinijinjaRenderBindInternal(input, true);
}

static string_t RenderToVector(Vector &result, const MinijinjaRenderBindData &bind_data,
                               vector<const char *> &autoescape_on_ptrs, string_t expression,
                               const char *context_json, idx_t context_json_len) {
	ResultCString eval_result =
	    render_template(expression.GetData(), expression.GetSize(), context_json, context_json_len,
	                    bind_data.template_path.c_str(), bind_data.autoescape, bind_data.undefined_behavior.c_str(),
	                    autoescape_on_ptrs.data(), static_cast<int32_t>(autoescape_on_ptrs.size()));
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

	// The keyword-only options follow the positional arguments in the chunk; their values were captured at bind.
	auto &expression_vector = args.data[0];

	vector<const char *> autoescape_on_ptrs;
	for (const auto &s : bind_data.autoescape_on) {
		autoescape_on_ptrs.push_back(s.c_str());
	}

	if (bind_data.has_context) {
		auto &context_json_vector = args.data[1];
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    expression_vector, context_json_vector, result, [&](string_t expression, string_t context_json) {
			    return RenderToVector(result, bind_data, autoescape_on_ptrs, expression, context_json.GetData(),
			                          context_json.GetSize());
		    });
	} else {
		UnaryExecutor::Execute<string_t, string_t>(expression_vector, result, [&](string_t expression) {
			return RenderToVector(result, bind_data, autoescape_on_ptrs, expression, "{}", 2);
		});
	}
}

static FunctionDescription MakeDescription(vector<string> names, vector<LogicalType> types, string description,
                                           vector<string> examples) {
	FunctionDescription desc;
	desc.description = std::move(description);
	// The keyword-only options, in the order AddRenderOptions declares them
	names.insert(names.end(), {"autoescape", "template_path", "undefined_behavior", "autoescape_extensions"});
	types.insert(types.end(), {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                           LogicalType::LIST(LogicalType::VARCHAR)});
	desc.parameter_names = std::move(names);
	desc.parameter_types = std::move(types);
	desc.examples = std::move(examples);
	desc.categories = {"text", "template"};
	return desc;
}

// Extension initalization.
static void LoadInternal(ExtensionLoader &loader) {
	// minijinja_render(template, *, autoescape, template_path, undefined_behavior, autoescape_extensions)
	{
		ScalarFunctionSet render("minijinja_render");

		ScalarFunction render_no_context({}, LogicalType::VARCHAR, MinijinjaRenderFunc, MinijinjaRenderBind);
		render_no_context.GetSignature().AddParameter("template", LogicalType::VARCHAR);
		AddRenderOptions(render_no_context);
		render_no_context.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		render_no_context.SetVolatile();
		render_no_context.SetFallible();
		render.AddFunction(render_no_context);

		CreateScalarFunctionInfo info(render);
		info.descriptions.push_back(
		    MakeDescription({"template"}, {LogicalType::VARCHAR},
		                    "Render a Jinja2-style template using the MiniJinja templating engine. Options are passed "
		                    "by name: autoescape (default true), template_path (directory to load templates from, "
		                    "default none), undefined_behavior (lenient, strict, chainable or semi_strict; default "
		                    "lenient) and autoescape_extensions (default [])",
		                    {"minijinja_render('Hello World!')",
		                     "minijinja_render('Hello & Goodbye!', autoescape := false)"}));
		loader.RegisterFunction(info);
	}

	// minijinja_render_with_context(template, context, *, autoescape, template_path, undefined_behavior,
	//                               autoescape_extensions)
	{
		ScalarFunctionSet render_with_ctx("minijinja_render_with_context");

		ScalarFunction render_with_context({}, LogicalType::VARCHAR, MinijinjaRenderFunc,
		                                   MinijinjaRenderWithContextBind);
		render_with_context.GetSignature()
		    .AddParameter("template", LogicalType::VARCHAR)
		    .AddParameter("context", LogicalType::JSON());
		AddRenderOptions(render_with_context);
		render_with_context.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		render_with_context.SetVolatile();
		render_with_context.SetFallible();
		render_with_ctx.AddFunction(render_with_context);

		CreateScalarFunctionInfo info(render_with_ctx);
		info.descriptions.push_back(MakeDescription(
		    {"template", "context"}, {LogicalType::VARCHAR, LogicalType::JSON()},
		    "Render a Jinja2-style template with a JSON context. Options are passed by name: autoescape (default "
		    "true), template_path (directory to load templates from, default none), undefined_behavior (lenient, "
		    "strict, chainable or semi_strict; default lenient) and autoescape_extensions (default [])",
		    {"minijinja_render_with_context('Hello {{ name }}!', '{\"name\": \"World\"}')",
		     "minijinja_render_with_context('{{ missing }}', '{}', undefined_behavior := 'strict')"}));
		loader.RegisterFunction(info);
	}

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
