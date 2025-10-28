# DuckDB [MiniJinja](https://docs.rs/minijinja/) Extension by [Query.Farm](https://query.farm)

The **MiniJinja** extension, developed by **[Query.Farm](https://query.farm)**, brings powerful template rendering capabilities directly to your SQL queries in DuckDB. Generate dynamic text, HTML, configuration files, and reports using the robust [MiniJinja](https://docs.rs/minijinja/latest/minijinja/) templating engine without leaving your database environment.

## Use Cases

The MiniJinja extension is perfect for:

- **Dynamic report generation**: Create custom formatted reports with data from your database
- **Configuration file generation**: Generate config files, scripts, and infrastructure-as-code templates
- **HTML/XML generation**: Build web pages, emails, and XML documents with database content
- **Data transformation**: Convert structured data into various text formats
- **Notification templates**: Create personalized messages, alerts, and communications
- **ETL pipeline outputs**: Transform data into specific formats required by downstream systems
- **Dynamic SQL generation**: Create parameterized queries and DDL statements
- **Internationalization**: Generate localized content using template variables

## Installation

**`minijinja` is a [DuckDB Community Extension](https://github.com/duckdb/community-extensions).**

You can now use this by using this SQL:

```sql
INSTALL minijinja FROM community;
LOAD minijinja;
```

## Functions

### `minijinja_render(template, context, ...options)`

Renders MiniJinja templates with JSON context data and customizable options.

**Basic Usage:**

```sql
-- Simple variable substitution
SELECT minijinja_render('{{ foo }}', '{"foo": "bar"}');
┌────────────────────────────────────────────┐
│ minijinja_render('{{ foo }}', '{"foo": "bar"}') │
│                  varchar                   │
├────────────────────────────────────────────┤
│ bar                                        │
└────────────────────────────────────────────┘

-- Template without context (will error if variables are referenced)
SELECT minijinja_render('Hello, World!');
┌──────────────────────────────┐
│ minijinja_render('Hello, World!') │
│           varchar            │
├──────────────────────────────┤
│ Hello, World!                │
└──────────────────────────────┘
```

**Advanced Context Usage:**

```sql
-- Complex JSON context with nested objects
SELECT minijinja_render(
    'Hello {{ user.name }}, you have {{ user.messages }} new messages!',
    '{"user": {"name": "Alice", "messages": 5}}'
) as output;
┌───────────────────────────────────────┐
│                output                 │
│                varchar                │
├───────────────────────────────────────┤
│ Hello Alice, you have 5 new messages! │
└───────────────────────────────────────┘

-- Using arrays and loops
SELECT minijinja_render(
    'Items: {% for item in items %}{{ item.name }}{% if not loop.last %}, {% endif %}{% endfor %}',
    '{"items": [{"name": "Apple"}, {"name": "Banana"}, {"name": "Cherry"}]}'
) as output;
┌──────────────────────────────┐
│            output            │
│           varchar            │
├──────────────────────────────┤
│ Items: Apple, Banana, Cherry │
└──────────────────────────────┘
```

**HTML Generation with Autoescaping:**

```sql
-- Default autoescaping (enabled)
SELECT minijinja_render('{{ v }}', '{"v": "B&O"}') as output;
┌─────────┐
│ output  │
│ varchar │
├─────────┤
│ B&amp;O │
└─────────┘

-- Disable autoescaping for raw output
SELECT minijinja_render('{{ v }}', '{"v": "B&O"}', autoescape := false) as output;
┌─────────┐
│ output  │
│ varchar │
├─────────┤
│ B&O     │
└─────────┘
```

**Template File Rendering:**

```sql
-- Render from template files with custom path
SELECT minijinja_render(
    'index.html',
    '{"v": "B&O"}',
    autoescape := false,
    template_path := './templates'
) as output;
┌─────────┐
│ output  │
│ varchar │
├─────────┤
│ B&O     │
└─────────┘
```

**Error Handling:**

```sql
-- Template errors are reported clearly
SELECT minijinja_render('{{ missing_var }}', undefined_behavior := 'strict');
Invalid Input Error:
Error rendering template: MiniJinja render error: Error { kind: UndefinedError, name: "<string>", line: 1 }

---------------------------------- <string> -----------------------------------
   1 > {{ missing_var }}
     i    ^^^^^^^^^^^ undefined value
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
No referenced variables
-------------------------------------------------------------------------------

-- File not found errors
SELECT minijinja_render('nonexistent.html', '{}', template_path := './templates/*.html');
Invalid Input Error:
Error rendering template: MiniJinja render error: Error { kind: TemplateNotFound, detail: "template \"nonexistent.html\" does not exist" }
```

**Parameters:**

- `template`: Template string or filename (when using `template_path`)
- `context`: Any object that can be coerced to JSON, most often should be a JSON map.
- `autoescape`: Boolean, enable/disable HTML autoescaping (default: `true`)
- `autoescape_on`: `VARCHAR[]`, A list of file extensions where autoescaping should be applied.
- `template_path`: Directory path for template files (enables file mode)
- `undefined_behavior`: The behavior of MiniJinja when an undefined variable is encountered can be `strict`, `lenient`, `chainable` or `semistrict`.  See the [definitions of each type of behavior](https://docs.rs/minijinja/latest/minijinja/enum.UndefinedBehavior.html).

**Template Syntax:**

The MiniJinja extension uses the full [MiniJinja templating language](https://docs.rs/minijinja/latest/minijinja/syntax/index.html), which includes:

- **Variables**: `{{ variable_name }}`
- **Filters**: `{{ name | upper }}`, `{{ price | round(precision=2) }}`
- **Control structures**:
  ```
  {% if condition %}...{% endif %}
  {% for item in list %}...{% endfor %}
  {% set var = value %}
  ```
- **Comments**: `{# This is a comment #}`
- **Template inheritance**: `{% extends "base.html" %}`, `{% block content %}...{% endblock %}`
- **Macros**: `{% macro button(text) %}...{% endmacro %}`

## Available Filters

MiniJinja includes many built-in [filters for data transformation](https://docs.rs/minijinja/latest/minijinja/filters/index.html).

## Contributing

The MiniJinja extension is open source and developed by [Query.Farm](https://query.farm). Contributions are welcome!

## License

[MIT License](LICENSE)
