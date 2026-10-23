# MiniJinja repository notes

For installation and the full user documentation, see the
[Query Farm MiniJinja documentation](https://query.farm/products/extensions/minijinja/).

## Render a context from columns

Use `minijinja_render_with_context` when supplying JSON context, including values
constructed from columns. `minijinja_render` is the entry point without context;
its additional option arguments must be constant expressions.

```sql
SELECT minijinja_render_with_context('Hello {{ name }}!', json_object('name', name))
FROM (VALUES ('Ada'), ('Grace')) AS users(name);
-- Hello Ada!
-- Hello Grace!
```
